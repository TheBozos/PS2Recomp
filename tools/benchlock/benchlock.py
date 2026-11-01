#!/usr/bin/env python3
"""Machine-wide lock for sessions sharing a box: benchmarks, game runs and compilations. See docs/benchlock.md.

Modes
  bench  exclusive: game runs whose timing matters (fps, profiles, A/B). Nothing else holds the lock.
  comp   shared: builds, recompiles and correctness game runs. Any number at once (or at most `config
         --comp-slots N`), never during a bench.

Waiters take the lock in arrival order, so a waiting bench holds back the comps that arrive after it and builds
cannot starve a bench. A waiter can ask the ones ahead of it to let it pass (`ask`); each of them decides for itself
(`yield`), and silence keeps the order.

--host NAME locks a remote build machine (an ssh name) instead of this one: a separate lock, the same journal. On a
remote host comp is exclusive too: one heavy job per box at a time.

Every hold is a lease with a deadline (bench max 30 min, comp max 90 min). The lock is a kernel lock held by a
process, so it is released when that process exits for any reason: the command finishes, is killed, hits its
deadline, or the session dies. Nothing has to be cleaned up by hand.

  benchlock run  -t TAG -m bench|comp -w WHY -e MIN [-d MIN] [--wait SECS] [--host H] -- CMD...
        Hold the lock while CMD runs. CMD is killed (whole process group / job) at the deadline.
  benchlock take -t TAG -m bench|comp -w WHY -e MIN [-d MIN] [--wait SECS] [--host H]
        Hold the lock from a background holder until `release` or the deadline. Prints the lease id.
  benchlock release --id ID | -t TAG   (-t only when that tag holds a single lease; refused otherwise)
  benchlock status                     holders, waiters in order, open requests to pass
  benchlock wait [--host H] [--wait SECS]
        Wait until nothing holds the lock, without taking it.
  benchlock ask --id ID -e MIN -w WHY  (for a waiter) ask the waiters ahead to let it pass; lapses in 10 min
  benchlock yield --id MINE --to ID    (for a waiter ahead of ID) let ID pass
  benchlock config [--comp-slots N]    show or set this box's comp slot limit (0: unlimited)

-e is the holder's honest estimate in minutes, shown so other sessions know when the box frees up. -d is the hard
deadline (default: twice the estimate, at least 5 min more), capped per mode. A waiter that asked to pass holds the
lock for the minutes it asked with, at most.

Linux uses flock and a forked holder; Windows (native CPython: python.org or MSYS2 UCRT64) uses byte-range locks, a
detached holder and a Job object. Every process sharing one lock must use the same kind of Python: MSYS2's
msys/cygwin Python and native Python do not see each other's locks.

--wait 0 fails at once when busy; the default waits up to 30 min. Exit 75 when the wait runs out.

State lives in $BENCHLOCK_STATE_DIR, else %LOCALAPPDATA%/ps2bench on Windows, else $XDG_STATE_HOME/ps2bench
(~/.local/state/ps2bench). Every take, release, deadline expiry, dead holder, request and yield is written to its
journal.log. $BENCHLOCK_ANNOUNCE, when set, is a command run in the background with each journal line as its last
argument (opt-in, off by default).
"""
import argparse
import ctypes
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import time

WINDOWS = os.name == "nt"
if WINDOWS:
    import msvcrt
    from ctypes import wintypes
else:
    import fcntl


def state_dir():
    if os.environ.get("BENCHLOCK_STATE_DIR"):
        return os.environ["BENCHLOCK_STATE_DIR"]
    if WINDOWS and os.environ.get("LOCALAPPDATA"):
        return os.path.join(os.environ["LOCALAPPDATA"], "ps2bench")
    return os.path.join(os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state"), "ps2bench")


STATE = state_dir()
LOCAL = "local"
LEASES = os.path.join(STATE, "leases")
QUEUE = os.path.join(STATE, "queue")
ASKS = os.path.join(STATE, "asks")
CONFIG = os.path.join(STATE, "config.json")
JOURNAL = os.path.join(STATE, "journal.log")
MAX = {"bench": 30 * 60, "comp": 90 * 60}
EX_TEMPFAIL = 75
ASK_LAPSE = 10 * 60
ASK_EVERY = 30 * 60
# Windows has no shared file lock: comp holds one of these bytes of the lock file, bench and remote hosts all of them.
SLOTS = 64


def log(msg):
    print(f"[benchlock] {msg}", file=sys.stderr, flush=True)


def journal(tag, text):
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {tag}: {text}"
    try:
        with open(JOURNAL, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass
    hook = os.environ.get("BENCHLOCK_ANNOUNCE")
    if hook:
        try:
            cmd = (hook + " " + subprocess.list2cmdline([line])) if WINDOWS else shlex.split(hook) + [line]
            subprocess.Popen(cmd, shell=WINDOWS, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=not WINDOWS)
        except (OSError, ValueError):
            pass


def clock(t):
    return time.strftime("%H:%M", time.localtime(t))


def minutes(m):
    return f"{m:g}"


def safe(name):
    return re.sub(r"[^A-Za-z0-9._-]", "_", name)


def host_of(entry):
    return entry.get("host", LOCAL)


def lock_name(lease):
    return f"{lease['mode']} lock" + ("" if host_of(lease) == LOCAL else f" on {host_of(lease)}")


def lease_text(lease):
    return (f"{lock_name(lease)} ({lease['why']}): expect ~{minutes(lease['expect'])} min, done around "
            f"{clock(lease['start'] + lease['expect'] * 60)}, hard deadline {clock(lease['deadline'])}")


def ended_text(lease, how):
    held = round((time.time() - lease["start"]) / 60, 1)
    return f"{how} the {lock_name(lease)} ({lease['why']}) after {held} min, expected {minutes(lease['expect'])}."


if WINDOWS:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    kernel32.CreateJobObjectW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR)
    kernel32.SetInformationJobObject.argtypes = (wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)
    kernel32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
    kernel32.TerminateJobObject.argtypes = (wintypes.HANDLE, wintypes.UINT)

    class IO_COUNTERS(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in
                    ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                     "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

    class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                    ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION), ("IoInfo", IO_COUNTERS),
                    ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]


def pid_alive(pid):
    if WINDOWS:
        # os.kill(pid, 0) would terminate the process on Windows.
        handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return ctypes.get_last_error() == 5  # ERROR_ACCESS_DENIED: exists, not ours
        code = wintypes.DWORD()
        ok = kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        kernel32.CloseHandle(handle)
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def die_with_parent():
    if sys.platform.startswith("linux"):
        ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGKILL)  # PR_SET_PDEATHSIG


class Job:
    """The job of a `run`: killed as a whole at the deadline, and with its holder when that dies."""

    def __init__(self, cmd):
        if WINDOWS:
            self.handle = kernel32.CreateJobObjectW(None, None)
            info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
            info.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            kernel32.SetInformationJobObject(self.handle, 9, ctypes.byref(info), ctypes.sizeof(info))
            self.proc = subprocess.Popen(cmd, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
            kernel32.AssignProcessToJobObject(self.handle, int(self.proc._handle))
        else:
            self.proc = subprocess.Popen(cmd, start_new_session=True, preexec_fn=die_with_parent)

    def signal(self, sig):
        if WINDOWS:
            kernel32.TerminateJobObject(self.handle, 1)
            return
        try:
            os.killpg(self.proc.pid, sig)
        except ProcessLookupError:
            pass

    def kill(self):
        self.signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.signal(getattr(signal, "SIGKILL", signal.SIGTERM))
            self.proc.wait()


def read_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def write_json(path, data):
    for attempt in range(50):
        try:
            with open(path + ".tmp", "w") as f:
                json.dump(data, f)
            os.replace(path + ".tmp", path)
            return
        except PermissionError:
            if attempt == 49:
                raise
            time.sleep(0.05)


def unlink(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


def live_entries(directory, on_dead=None):
    out = []
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".json"):
            continue
        path = os.path.join(directory, name)
        entry = read_json(path)
        if entry is None:
            continue
        if pid_alive(entry["pid"]):
            out.append(entry)
        else:
            unlink(path)
            if on_dead:
                on_dead(entry)
    return out


def leases():
    return live_entries(LEASES, lambda l: journal(l["tag"], ended_text(l, "benchlock: holder died, freed")))


def waiters(host=None):
    out = live_entries(QUEUE)
    if host is not None:
        out = [w for w in out if host_of(w) == host]
    return sorted(out, key=lambda w: (w["order"], w["pid"]))


def open_asks():
    out = {}
    now = time.time()
    for name in os.listdir(ASKS):
        if not name.endswith(".json"):
            continue
        path = os.path.join(ASKS, name)
        ask = read_json(path)
        if ask is None:
            continue
        if now - ask["time"] > ASK_LAPSE or not pid_alive(ask["pid"]):
            unlink(path)
            continue
        out[ask["id"]] = ask
    return out


def ask_path(lease_id):
    return os.path.join(ASKS, safe(lease_id) + ".json")


def comp_slots():
    config = read_json(CONFIG) or {}
    return int(config.get("comp_slots", 0))


def lock_paths(host):
    if host == LOCAL:
        return os.path.join(STATE, "gate"), os.path.join(STATE, "lock")
    return os.path.join(STATE, f"gate@{safe(host)}"), os.path.join(STATE, f"lock@{safe(host)}")


def describe(lease, now):
    left = int(lease["deadline"] - now)
    held = int(now - lease["start"])
    where = "" if host_of(lease) == LOCAL else f"@{host_of(lease)}"
    return (f"{lease['mode']:5}{where} {lease['tag']} - {lease['why']} (id {lease['id']}, pid {lease['pid']}, "
            f"held {held // 60}m{held % 60:02d}s, expected {minutes(lease['expect'])}m, done around "
            f"{clock(lease['start'] + lease['expect'] * 60)}, hard deadline in {left // 60}m{left % 60:02d}s)")


def describe_waiter(position, waiter, asks, now):
    waited = int(now - waiter["since"])
    where = "" if host_of(waiter) == LOCAL else f"@{host_of(waiter)}"
    text = (f"{position}. {waiter['mode']:5}{where} {waiter['tag']} - {waiter['why']} (id {waiter['id']}, "
            f"waiting {waited // 60}m{waited % 60:02d}s, expects {minutes(waiter['expect'])}m)")
    ask = asks.get(waiter["id"])
    if ask:
        text += f", asks to pass: ~{minutes(ask['minutes'])} min, {ask['why']}"
    return text


def wait_until(attempt, deadline, what):
    announced = False
    while True:
        if attempt():
            return True
        if time.time() >= deadline:
            return False
        if not announced:
            holders = leases()
            now = time.time()
            log(f"waiting for {what}; held by: " + ("; ".join(describe(l, now) for l in holders) or "(starting)"))
            announced = True
        time.sleep(0.5)


def try_flock(fd, op):
    try:
        fcntl.flock(fd, op | fcntl.LOCK_NB)
        return True
    except BlockingIOError:
        return False


def try_range(fd, start, length):
    os.lseek(fd, start, os.SEEK_SET)
    try:
        msvcrt.locking(fd, msvcrt.LK_NBLCK, length)
        return True
    except OSError:
        return False


def unlock(fd, start=0, length=0):
    if WINDOWS:
        os.lseek(fd, start, os.SEEK_SET)
        msvcrt.locking(fd, msvcrt.LK_UNLCK, length)
    else:
        fcntl.flock(fd, fcntl.LOCK_UN)


class Held:
    def __init__(self):
        self.parts = []

    def add(self, fd, start=0, length=0):
        self.parts.append((fd, start, length))

    def release(self):
        for fd, start, length in reversed(self.parts):
            unlock(fd, start, length)
            os.close(fd)
        self.parts = []


def open_lock(path):
    return os.open(path, os.O_RDWR | os.O_CREAT | getattr(os, "O_BINARY", 0), 0o666)


def try_lock(lock, exclusive, slots, slot_paths):
    """One non-blocking attempt; the Held it returns owns lock."""
    held = Held()
    if WINDOWS:
        count = min(slots, SLOTS) if slots else SLOTS
        for start, length in [(0, SLOTS)] if exclusive else [(i, 1) for i in range(count)]:
            if try_range(lock, start, length):
                held.add(lock, start, length)
                return held
        return None
    if not try_flock(lock, fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH):
        return None
    held.add(lock)
    if exclusive or not slots:
        return held
    for path in slot_paths:
        slot = open_lock(path)
        if try_flock(slot, fcntl.LOCK_EX):
            held.add(slot)
            return held
        os.close(slot)
    unlock(lock)
    return None


def acquire(mode, wait, host, ticket=None):
    """Returns a Held lock, or None when the wait ran out. With a ticket, waits for its turn in arrival order."""
    give_up = time.time() + wait
    gate_path, lock_path = lock_paths(host)
    exclusive = mode == "bench" or host != LOCAL
    slots = 0 if host != LOCAL else comp_slots()
    slot_paths = [os.path.join(STATE, f"slot{i}") for i in range(slots)]
    gate = open_lock(gate_path)
    lock = open_lock(lock_path)
    gated = []
    result = []

    def my_turn():
        if ticket is None:
            return True
        queue = waiters(host)
        return not queue or queue[0]["id"] == ticket["id"]

    def attempt():
        if not my_turn():
            if gated:
                unlock(gate, 0, 1)
                gated.clear()
            return False
        if not gated:
            if not (try_range(gate, 0, 1) if WINDOWS else try_flock(gate, fcntl.LOCK_EX)):
                return False
            gated.append(True)
        held = try_lock(lock, exclusive, slots, slot_paths)
        if held:
            result.append(held)
        return bool(held)

    try:
        wait_until(attempt, give_up, "the lock")
        return result[0] if result else None
    finally:
        if gated:
            unlock(gate, 0, 1)
        os.close(gate)
        if not result:
            os.close(lock)


def release_requested(path):
    return os.path.exists(path[:-len(".json")] + ".release")


def new_entry(args):
    pid = os.getpid()
    return {"id": f"{args.tag}-{pid}", "pid": pid, "tag": args.tag, "mode": args.mode, "why": args.why,
            "host": args.host, "expect": args.expect}


def enqueue(args):
    ticket = dict(new_entry(args), order=time.time_ns(), since=time.time())
    path = os.path.join(QUEUE, f"{ticket['pid']}.json")
    write_json(path, ticket)
    return ticket, path


def write_lease(args, deadline):
    lease = dict(new_entry(args), start=time.time(), deadline=deadline)
    path = os.path.join(LEASES, f"{os.getpid()}.json")
    write_json(path, lease)
    return lease, path


def duration(args):
    mins = args.deadline if args.deadline is not None else max(2 * args.expect, args.expect + 5)
    mins = max(mins, args.expect)
    if mins * 60 > MAX[args.mode]:
        log(f"deadline capped at {MAX[args.mode] // 60} min for {args.mode}")
        mins = MAX[args.mode] / 60
    return mins * 60


def take_lock(args):
    """Queues, acquires and writes the lease; returns (Held, lease, path) or None when the wait ran out."""
    ticket, ticket_path = enqueue(args)
    try:
        lock = acquire(args.mode, args.wait, args.host, ticket)
    finally:
        unlink(ticket_path)
    if lock is None:
        return None
    seconds = duration(args)
    ask = read_json(ask_path(ticket["id"]))
    if ask and time.time() - ask["time"] <= ASK_LAPSE and ask["minutes"] * 60 < seconds:
        seconds = ask["minutes"] * 60
        args.expect = min(args.expect, ask["minutes"])
    unlink(ask_path(ticket["id"]))
    lease, path = write_lease(args, time.time() + seconds)
    journal(args.tag, f"took the {lease_text(lease)}.")
    return lock, lease, path


def cmd_run(args):
    if not args.cmd:
        sys.exit("benchlock run: no command after --")
    taken = take_lock(args)
    if taken is None:
        log("gave up waiting")
        return EX_TEMPFAIL
    lock, lease, path = taken
    deadline = lease["deadline"]
    log(f"acquired {describe(lease, time.time())}")
    ended = "released"
    job = Job(args.cmd)

    def forward(sig, _frame):
        job.signal(sig)

    for name in ("SIGINT", "SIGTERM", "SIGHUP", "SIGBREAK"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), forward)
    try:
        while True:
            try:
                return job.proc.wait(timeout=min(1.0, max(0.0, deadline - time.time())))
            except subprocess.TimeoutExpired:
                if release_requested(path):
                    log(f"release requested, killing {args.cmd[0]}")
                    ended = "release requested, killed the job and released"
                    job.kill()
                    return 143
                if time.time() < deadline:
                    continue
                log(f"deadline reached, killing {args.cmd[0]}")
                ended = "deadline reached, killed the job and released"
                job.kill()
                return 124
    finally:
        unlink(path)
        unlink(path[:-len(".json")] + ".release")
        lock.release()
        journal(args.tag, ended_text(lease, ended[0].upper() + ended[1:]))
        log(f"released {lease['id']}")


def take_reply(reply):
    if not reply:
        log("holder died before acquiring")
        return 1
    if reply.startswith("timeout"):
        log("gave up waiting")
        return EX_TEMPFAIL
    print(reply)
    return 0


def hold(args, reply_fd):
    """The holder of a `take`: acquires, answers on reply_fd, then holds until release or the deadline."""
    devnull = os.open(os.devnull, os.O_RDWR)
    os.dup2(devnull, 0)
    released = False

    def stop(_sig, _frame):
        nonlocal released
        released = True

    taken = take_lock(args)
    os.dup2(devnull, 1)
    os.dup2(devnull, 2)
    if taken is None:
        os.write(reply_fd, b"timeout")
        return
    lock, lease, path = taken
    signal.signal(signal.SIGTERM, stop)
    for name in ("SIGHUP", "SIGINT", "SIGBREAK"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), signal.SIG_IGN)
    deadline = lease["deadline"]
    os.write(reply_fd, f"{lease['id']} ({lease['mode']}, until "
                       f"{time.strftime('%H:%M:%S', time.localtime(deadline))})".encode())
    os.close(reply_fd)
    while not released and time.time() < deadline:
        released = released or release_requested(path)
        time.sleep(min(0.5, max(0.0, deadline - time.time())))
    unlink(path)
    unlink(path[:-len(".json")] + ".release")
    lock.release()
    journal(args.tag, ended_text(lease, "Released" if released else "Lease deadline reached, released"))


def cmd_take(args):
    if WINDOWS:
        flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
        holder = subprocess.Popen([sys.executable, os.path.abspath(__file__), "_hold", json.dumps(vars(args))],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, creationflags=flags)
        return take_reply(holder.stdout.read().decode())
    read_end, write_end = os.pipe()
    if os.fork():
        os.close(write_end)
        return take_reply(os.read(read_end, 4096).decode())
    os.close(read_end)
    os.setsid()
    try:
        hold(args, write_end)
    finally:
        os._exit(0)


def cmd_release(args):
    targets = [l for l in leases() if (args.id and l["id"] == args.id) or (args.tag and l["tag"] == args.tag)]
    if not targets:
        log("nothing to release")
        return 1
    if not args.id and len(targets) > 1:
        log(f"{len(targets)} leases hold tag {args.tag}; release yours with --id: "
            + ", ".join(f"{l['id']} ({l['why']})" for l in targets))
        return 2
    for lease in targets:
        open(os.path.join(LEASES, f"{lease['pid']}.release"), "w").close()
        if not WINDOWS:
            os.kill(lease["pid"], signal.SIGTERM)
        for _ in range(600):
            if not pid_alive(lease["pid"]):
                break
            time.sleep(0.1)
        log(f"released {lease['id']}")
    return 0


def cmd_status(_args):
    now = time.time()
    held = leases()
    queued = waiters()
    asks = open_asks()
    if not held and not queued:
        print("free")
    for lease in held:
        print(describe(lease, now))
    for position, waiter in enumerate(queued, 1):
        print("queued " + describe_waiter(position, waiter, asks, now))
    slots = comp_slots()
    if slots:
        print(f"comp slots: {slots}")
    return 0


def cmd_wait(args):
    gate_path, lock_path = lock_paths(args.host)
    lock = open_lock(lock_path)
    try:
        def free():
            if WINDOWS:
                if not try_range(lock, 0, SLOTS):
                    return False
                unlock(lock, 0, SLOTS)
                return True
            if not try_flock(lock, fcntl.LOCK_EX):
                return False
            unlock(lock)
            return True

        if not wait_until(free, time.time() + args.wait, "the lock to be free"):
            log("gave up waiting")
            return EX_TEMPFAIL
    finally:
        os.close(lock)
    return 0


def find_waiter(lease_id):
    for waiter in waiters():
        if waiter["id"] == lease_id:
            return waiter
    return None


def cmd_ask(args):
    waiter = find_waiter(args.id)
    if waiter is None:
        log(f"{args.id} is not waiting")
        return 1
    if args.id in open_asks():
        log(f"{args.id} already has an open request")
        return 1
    last = os.path.join(ASKS, safe(waiter["tag"]) + ".last")
    if os.path.exists(last) and time.time() - os.path.getmtime(last) < ASK_EVERY:
        log(f"{waiter['tag']} asked less than {ASK_EVERY // 60} min ago")
        return 1
    queue = waiters(host_of(waiter))
    ahead = [w["id"] for w in queue[:queue.index(waiter)]]
    if not ahead:
        log(f"nobody is waiting ahead of {args.id}")
        return 1
    write_json(ask_path(args.id), {"id": args.id, "pid": waiter["pid"], "tag": waiter["tag"],
                                   "minutes": args.expect, "why": args.why, "time": time.time()})
    open(last, "w").close()
    journal(waiter["tag"], f"{args.id} asks to pass in the queue: ~{minutes(args.expect)} min, {args.why}. "
                           f"Waiting ahead: {', '.join(ahead)}. Each can let it pass with "
                           f"`benchlock yield --id <yours> --to {args.id}`; silence keeps the order, the request "
                           f"lapses in {ASK_LAPSE // 60} min.")
    print(f"asked; ahead: {', '.join(ahead)}")
    return 0


def cmd_yield(args):
    mine = find_waiter(args.id)
    theirs = find_waiter(args.to)
    if mine is None or theirs is None:
        log(f"{args.id if mine is None else args.to} is not waiting")
        return 1
    if args.to not in open_asks():
        log(f"{args.to} has no open request")
        return 1
    if host_of(mine) != host_of(theirs):
        log(f"{args.id} and {args.to} wait for different locks")
        return 1
    if (theirs["order"], theirs["pid"]) < (mine["order"], mine["pid"]):
        log(f"{args.to} is already ahead of {args.id}")
        return 1
    theirs["order"] = mine["order"] - 1
    write_json(os.path.join(QUEUE, f"{theirs['pid']}.json"), theirs)
    journal(mine["tag"], f"{args.id} lets {args.to} pass in the queue.")
    return 0


def cmd_config(args):
    if args.comp_slots is not None:
        if args.comp_slots < 0:
            sys.exit("benchlock config: --comp-slots must be 0 (unlimited) or more")
        config = read_json(CONFIG) or {}
        config["comp_slots"] = args.comp_slots
        write_json(CONFIG, config)
        journal("benchlock", f"comp slots set to {args.comp_slots or 'unlimited'}.")
    slots = comp_slots()
    print(f"comp slots: {slots or 'unlimited'}")
    return 0


def parser():
    p = argparse.ArgumentParser(prog="benchlock", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="action", required=True)
    for name in ("run", "take"):
        s = sub.add_parser(name)
        s.add_argument("-t", "--tag", required=True)
        s.add_argument("-m", "--mode", required=True, choices=("bench", "comp"))
        s.add_argument("-w", "--why", required=True)
        s.add_argument("-e", "--expect", type=float, required=True, help="expected minutes, shown to others")
        s.add_argument("-d", "--deadline", type=float, help="hard deadline in minutes")
        s.add_argument("--wait", type=float, default=30 * 60, help="seconds to wait for the lock")
        s.add_argument("--host", default=LOCAL, help="remote build machine to lock instead of this one")
        if name == "run":
            s.add_argument("cmd", nargs=argparse.REMAINDER)
    s = sub.add_parser("release")
    g = s.add_mutually_exclusive_group(required=True)
    g.add_argument("-t", "--tag")
    g.add_argument("--id")
    sub.add_parser("status")
    s = sub.add_parser("wait")
    s.add_argument("--host", default=LOCAL)
    s.add_argument("--wait", type=float, default=30 * 60, help="seconds to wait")
    s = sub.add_parser("ask")
    s.add_argument("--id", required=True, help="the waiting lease id (see status)")
    s.add_argument("-e", "--expect", type=float, required=True, help="minutes needed once it has the lock")
    s.add_argument("-w", "--why", required=True)
    s = sub.add_parser("yield")
    s.add_argument("--id", required=True, help="your waiting lease id")
    s.add_argument("--to", required=True, help="the waiting lease id to let pass")
    s = sub.add_parser("config")
    s.add_argument("--comp-slots", type=int, help="most comp holders at once on this box (0: unlimited)")
    return p


def main():
    for directory in (LEASES, QUEUE, ASKS):
        os.makedirs(directory, exist_ok=True)
    if len(sys.argv) == 3 and sys.argv[1] == "_hold":
        hold(argparse.Namespace(**json.loads(sys.argv[2])), os.dup(sys.stdout.fileno()))
        return 0
    args = parser().parse_args()
    if getattr(args, "cmd", None) and args.cmd[0] == "--":
        args.cmd = args.cmd[1:]
    return {"run": cmd_run, "take": cmd_take, "release": cmd_release, "status": cmd_status, "wait": cmd_wait,
            "ask": cmd_ask, "yield": cmd_yield, "config": cmd_config}[args.action](args)


if __name__ == "__main__":
    sys.exit(main())
