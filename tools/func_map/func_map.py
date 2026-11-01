#!/usr/bin/env python3
"""PS2Recomp function map: every EE function of a port's game ELF and every recompiled IRX function, coloured by how
far it is from running without emulation.

    python3 tools/func_map/func_map.py --config <port>.json [--out DIR] [--coverage FILE] [--check]

The port's JSON config names the game ELF, the recompiler config (stubs, entry points, [mmio]), the generated tree
(function list, stub bindings, generated/iop/), the port's runtime sources (native overrides) and optional VU program
specs, runtime badges and library patterns; PS2Recomp's own runtime sources give the stub handlers and the syscall
dispatcher. Writes map.json, map.html, map.svg into --out and appends the counts to history.json there (or
--history). Everything but map.svg names game addresses: keep it out of the port's repository. Standard library
only (Python 3.8+), Linux and Windows. See docs/func_map.md.
"""

import argparse
import bisect
import datetime
import glob
import html
import json
import os
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

TOOL_VERSION = 2
FORK_ROOT = Path(__file__).resolve().parent.parent.parent

NATIVE, CLEAN, DIRECT, INDIRECT, STUB = range(5)
STATES = [
    ("native", "Native C++", "Hand-written or lifted C++ runs instead of the game's code (game overrides, VU programs)."),
    ("clean", "Recompiled, clean", "Generated C++ from the MIPS; no emulated hardware touched by the function or its callees."),
    ("direct", "Touches emulation", "The function itself reads or writes emulated hardware (or its VU0 micro mode): the to-do list."),
    ("indirect", "Calls emulation", "Calls, one or two levels down, a function that touches emulation or an emulation-backed stub."),
    ("stub", "Runtime stub / HLE", "Library function the runtime answers by name (or a kernel syscall wrapper)."),
]

FLAG_UNCERTAIN = 1
FLAG_EMU_STUB = 2
FLAG_RAN = 4
FLAG_WRAPPER = 8

DMA_CHANNELS = [
    (0x10008000, "D0 VIF0"), (0x10009000, "D1 VIF1"), (0x1000A000, "D2 GIF"), (0x1000B000, "D3 fromIPU"),
    (0x1000B400, "D4 toIPU"), (0x1000C000, "D5 SIF0"), (0x1000C400, "D6 SIF1"), (0x1000C800, "D7 SIF2"),
    (0x1000D000, "D8 fromSPR"), (0x1000D400, "D9 toSPR"),
]
DMA_REGS = {0x00: "CHCR", 0x10: "MADR", 0x20: "QWC", 0x30: "TADR", 0x40: "ASR0", 0x50: "ASR1", 0x80: "SADR"}
HW_REGIONS = [
    (0x10000000, 0x10002000, "timer", "EE timers"),
    (0x10002000, 0x10003000, "ipu", "IPU"),
    (0x10003000, 0x10003800, "gif", "GIF"),
    (0x10003800, 0x10003C00, "vif", "VIF0"),
    (0x10003C00, 0x10004000, "vif", "VIF1"),
    (0x10004000, 0x10005000, "fifo", "VIF0 FIFO"),
    (0x10005000, 0x10006000, "fifo", "VIF1 FIFO"),
    (0x10006000, 0x10007000, "fifo", "GIF FIFO"),
    (0x10007000, 0x10008000, "fifo", "IPU FIFO"),
    (0x10008000, 0x1000E000, "dma", "DMA channel"),
    (0x1000E000, 0x1000F000, "dmac", "DMAC control"),
    (0x1000F000, 0x1000F100, "intc", "INTC"),
    (0x1000F200, 0x1000F300, "sif", "SIF registers"),
    (0x1000F500, 0x1000F600, "dmac", "DMAC enable"),
    (0x10000000, 0x10010000, "eeio", "EE I/O"),
    (0x11000000, 0x11004000, "vumem", "VU0 micro memory"),
    (0x11004000, 0x11008000, "vumem", "VU0 data memory"),
    (0x11008000, 0x1100C000, "vumem", "VU1 micro memory"),
    (0x1100C000, 0x11010000, "vumem", "VU1 data memory"),
    (0x12000000, 0x12001000, "gs", "GS privileged"),
    (0x12001000, 0x12002000, "gs", "GS CSR/IMR"),
]
POLL_REGS = {0x1000E010: "D_STAT", 0x12001000: "GS CSR", 0x1000F000: "I_STAT", 0x1000F230: "SIF SMFLG"}
VU_CONTROL = {27: "CMSAR0", 28: "FBRST", 29: "VPU-STAT", 31: "CMSAR1"}
TOUCH_KINDS = {
    "dma": "DMA channel registers", "dmac": "DMAC control", "gif": "GIF registers", "vif": "VIF registers",
    "fifo": "hardware FIFO", "gs": "GS privileged registers", "vumem": "VU memory", "timer": "EE timers",
    "intc": "INTC", "sif": "SIF registers", "ipu": "IPU", "eeio": "other EE I/O",
    "vu0micro": "VU0 micro mode (VCALLMS)", "vuctl": "VU control registers", "tag": "VIF code / DMA tag constant",
    "addr": "hardware address passed on", "mmio": "analyzer-resolved MMIO",
}

EMU_KEYWORDS = [
    ("I/O registers", r"0x1[0-2]00(?!0000)[0-9A-Fa-f]{4}|IORegister"),
    ("DMAC model", r"[Dd]mac|DMAC|processDma|[Dd]maChannel|CHCR|Chcr"),
    ("GIF", r"\bgif\w*|\bGif(?!Pk)\w*|GIF"),
    ("VIF", r"\bvif\d?\w*|\bVif\d?\w*|VIF"),
    ("GS model", r"\bgs\(\)|\.gs\(|GsMem|gsMemory|\bm_gs\b|\bCSR\b|\bIMR\b"),
    ("VU1", r"vu1|Vu1|VU1"),
    ("IOP / SIF", r"\biop\w*|\bIop\w*|IOP|\bsif\w*|\bSif\w*|SIF|\brpc\w*|Rpc\b"),
    ("SPU2", r"spu2|Spu2|SPU2"),
    ("interrupt model", r"addIrqHandler|[Ii]rqHandler|raiseIrq|[Ii]ntc|INTC"),
]
WEAK_KEYWORDS = [("waits on the emulated vblank", r"waitVSync|currentVSyncTick|vsyncTick|[Vv][Bb]lank")]

LIBRARIES = [
    ("libmpeg / libipu", r"^_?sceMpeg|^_?sceIpu|^_(clearOnce|sendData|clearEach|getpic|updateRef|decodeOr|decPicture|cpr|dispRef|doC|ch3dma|dualPrime|getAll|motion|getRef|doMC|slice|decMB|waitIpu|skipMB|copyAdd|copyRef|rix|ri0|initSeq|sequence|groupOf|picture|quantMatrix|extensionAnd|copyright)|^dmaRefImage|^receiveData|^flushByte|^setD[34]_CHCR"),
    ("libm", r"^(ceilf|cosf|floorf|sinf|atanf|fabsf|__ieee754|__kernel_)"),
    ("libdma", r"^(WaitDma|WatchDma|CheckAddress|memclr|sceDma)"),
    ("libgraph / libvu0 / libdev", r"^(sceGs|sceGif|sceVu|sceDev)"),
    ("libpad", r"^(scePad|_pad_|_send_to_iop)"),
    ("libsdr", r"^(sceSd|_sdr)"),
    ("libcdvd / libscf", r"^(_?sceCd|cmd_sem|cdvd_|PowerOffCB|ret1|GetRomName|IsT10K|sceScf|tobcd|frombcd|adddate|subdate|addhour|subhour|AdjustTime)"),
    ("libmc", r"^(sceMc|mce)"),
    ("libpc / libgcc / C++ runtime", r"^(scePc|__muldi|__divdi|__moddi|__udivdi|__umoddi|__float|__fix|__throw|__rethrow|new_eh|eh_|__get_|init_reg|__eh_|__sj|old_find|find_|get_reg|copy_reg|next_stack|throw_helper|fptodp|_fpadd|dp|litodp|__make|decode_|fde_|end_fde|__register|__deregister|count_fdes|add_fdes|search_fdes|frame_init|extract_cie|execute_cfa|__frame|__unpack|__pack|__fpcmp)"),
    ("newlib libc", r"^(atexit|atoi|exit|malloc|free|_malloc|memcmp|memcpy|memset|__malloc|_init_signal|_signal|_raise|__sigtramp|_sprintf|sscanf|str|_str|__sprint|__sbprintf|_vfprintf|cvt|exponent|__svfscanf|__sccl|vsprintf|__swsetup|quorem|_dtoa|fflush|std|__sf|__sinit|fread|_free|__sfvwrite|_fwalk|_setlocale|__smakebuf|_mbtowc|memchr|memmove|_Balloc|_Bfree|_multadd|_s2b|_hi0|_lo0|_i2b|_multiply|_pow5|_lshift|__mcmp|__mdiff|_ulp|_b2d|_d2b|_ratio|_mprec|lflush|__srefill|__sread|__swrite|__sseek|__submore|ungetc|_calloc|_realloc)"),
    ("libkernel", r"^(VSync|write$|read$|sbrk|kill|_sceSDC|_sceIDC|topThread|InitThread|iWakeup|iRotate|iSuspend|DelayThread|StartThread|_DelayThread|QueuePeek|sceTty|kputchar|deci2|serialPutchar|ftoi|printfloat|_sceSnprintf|snprintf|sceVsnprintf|sceSnprintf|sceVprintf|_?i?sceSif|_sceRpc|_request|_alarm|_search_svdata|_sceFs|use_iob|new_iob|sceFs|get_iob|scePower|sceOpen|sceClose|sceLseek|sceRead|sceWrite|sceIoctl|_sceCallCode|sceMkdir|sceFormat|sceAddDrv|sceD|sceGetstat|sceChstat|sceRename|sceSync|sceMount|sceSymlink|sceReadlink|_lf_bind|DIntr|supplement|kCopy|kFind|GetSystemCall|InitSystemCall|PatchIsNeeded|InitExecPS2|SetArg|ExecPS2|LoadExecPS2|ExecOSD|InitAlarm|SetT2|InitTimer|EndTimer|GetTimer|StartTimer|StopTimer|SetNextComp|InsertAlarm|UnlinkAlarm|cbTimer|iGet|iAlloc|AllocTimer|iFree|iStart|iStop|SetTimer|iSet|ReleaseTimer|iRelease|kputs|SetTLB|SetDebug|InitTLB|_kTLB|_kExit|_kDebug)"),
]


def hx(value):
    return f"0x{value:06x}"


def read_text(path):
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def git(cwd, *args):
    try:
        out = subprocess.run(["git", "-C", str(cwd), *args], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return ""
    return out.stdout.strip() if out.returncode == 0 else ""


DEFAULT_OVERRIDE_PATTERNS = [
    r"\b(?:registerFunction|replaceFunction)\(\s*(?P<target>[^,()]+?)\s*,\s*(?P<handler>&?\s*[A-Za-z_][\w:]*(?:<[^;]*?>)?|\[)",
]
DEFAULT_WRAPPER_PATTERNS = [
    r"\bg_\w+\s*\(\s*rdram\s*,\s*ctx\s*,\s*runtime\s*\)",
    r"\bs\.\w+\s*\(\s*rdram\s*,\s*ctx\s*,\s*runtime\s*\)",
]


class PortConfig:
    """The port's JSON config. Relative paths are taken from the config file's directory; ~ and ${VAR} expand. A path
    key may hold a list: the first candidate that exists wins."""

    def __init__(self, path):
        self.path = Path(path).resolve()
        self.base = self.path.parent
        try:
            self.data = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise SystemExit(f"{path}: {error}")
        if not isinstance(self.data, dict):
            raise SystemExit(f"{path}: the config is a JSON object")

    def get(self, key, default=None):
        return self.data.get(key, default)

    def resolve(self, value, base=None):
        text = os.path.expanduser(os.path.expandvars(str(value)))
        p = Path(text)
        return p if p.is_absolute() else (base or self.base) / p

    def path_of(self, key, override=None, must_exist=True):
        if override:
            return Path(override)
        value = self.data.get(key)
        if value is None:
            return None
        candidates = value if isinstance(value, list) else [value]
        resolved = [self.resolve(c) for c in candidates]
        for p in resolved:
            if p.exists():
                return p
        return None if must_exist else (resolved[0] if resolved else None)

    def paths_of(self, key):
        out = []
        for value in self.data.get(key, []):
            text = str(self.resolve(value))
            hits = sorted(glob.glob(text, recursive=True)) if any(c in text for c in "*?[") else [text]
            out.extend(Path(h) for h in hits)
        return out

    def title(self):
        game_id = self.data.get("game_id", "")
        name = self.data.get("title", game_id or "PS2 game")
        return f"{name} ({game_id})" if game_id and game_id not in name else name

    def short(self):
        return self.data.get("short_name") or self.data.get("game_id") or "port"


class Elf:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            raise SystemExit(f"{path}: not a 32-bit little-endian ELF")
        shoff, = struct.unpack_from("<I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
        raw = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
        strtab = raw[shstrndx][4]
        self.sections = []
        for name, typ, flags, addr, off, size, *_ in raw:
            end = d.index(b"\0", strtab + name)
            label = d[strtab + name:end].decode("ascii", "replace")
            if addr and size and typ != 8:
                self.sections.append({"name": label, "addr": addr, "size": size, "off": off, "exec": bool(flags & 4)})

    def section_of(self, addr):
        for s in self.sections:
            if s["addr"] <= addr < s["addr"] + s["size"]:
                return s
        return None

    def words(self, start, end):
        s = self.section_of(start)
        if not s:
            return []
        end = min(end, s["addr"] + s["size"])
        off = s["off"] + start - s["addr"]
        count = max(0, (end - start) // 4)
        return list(struct.unpack_from(f"<{count}I", self.data, off))


def load_recomp_config(path):
    text = read_text(path) if path else ""
    if not text:
        print(f"no recompiler config ({path}): no stub or entry-point names, no [mmio] table", file=sys.stderr)
        return {"stubs": [], "entries": [], "mmio": {}}
    try:
        import tomllib
        data = tomllib.loads(text)
        stubs = data.get("general", {}).get("stubs", [])
        entries = data.get("general", {}).get("entry_points", [])
        mmio = data.get("mmio", {})
    except Exception:
        stubs = re.findall(r'"([^"]+@0x[0-9A-Fa-f]+)"', text.split("entry_points", 1)[0])
        entries = re.findall(r'"([^"]+@0x[0-9A-Fa-f]+)"', text.split("entry_points", 1)[-1].split("[mmio]", 1)[0])
        mmio = dict(re.findall(r'^"(0x[0-9a-fA-F]+)"\s*=\s*"(0x[0-9a-fA-F]+)"', text, re.M))

    def pairs(items):
        out = []
        for item in items:
            name, _, addr = item.partition("@")
            if addr:
                out.append((name, int(addr, 16)))
        return out

    return {
        "stubs": pairs(stubs),
        "entries": pairs(entries),
        "mmio": {int(k, 16): int(v, 16) for k, v in mmio.items()},
    }


def load_functions(generated, config, elf, csv_path):
    """Function starts, ends and stub bindings from the generated tree, else from a name,address CSV (Ghidra export)."""
    funcs = {}
    source = None
    if generated and Path(generated).is_dir():
        source = f"generated tree ({generated})"
        head_re = re.compile(r"// Function: (\S+)\n// Address: 0x([0-9a-f]+) - 0x([0-9a-f]+)")
        stub_re = re.compile(r"(ps2_stubs|ps2_syscalls)::(\w+)\(")
        for entry in os.scandir(generated):
            if not entry.name.endswith(".cpp") or not entry.is_file():
                continue
            with open(entry.path, encoding="utf-8", errors="replace") as fh:
                head = fh.read(4096)
            m = head_re.search(head)
            if m:
                start, end = int(m.group(2), 16), int(m.group(3), 16)
                funcs[start] = {"addr": start, "end": end, "gen": m.group(1), "kind": "recompiled"}
                continue
            name = re.match(r"(.+)_0x([0-9a-f]+)\.cpp$", entry.name)
            stub = stub_re.search(head)
            if name and stub:
                start = int(name.group(2), 16)
                funcs[start] = {"addr": start, "end": None, "gen": name.group(1), "kind": "stub",
                                "handler": stub.group(2), "ns": stub.group(1)}
    else:
        if not csv_path:
            raise SystemExit("no generated tree and no function_list_csv in the config: no function list")
        source = f"{csv_path.name} (no generated tree: function ends and stub bindings approximated)"
        for line in read_text(csv_path).splitlines()[1:]:
            parts = line.split(",")
            if len(parts) >= 2 and re.match(r"\s*(0x)?[0-9A-Fa-f]+\s*$", parts[1]):
                start = int(parts[1], 16)
                funcs.setdefault(start, {"addr": start, "end": None, "gen": parts[0], "kind": "recompiled"})
        for name, addr in config["stubs"]:
            funcs[addr] = {"addr": addr, "end": None, "gen": name, "kind": "stub", "handler": name, "ns": "ps2_stubs"}
    names = {}
    for name, addr in config["entries"] + config["stubs"]:
        if not name.startswith(("sub_", "entry_", "FUN_")):
            names[addr] = name
    starts = sorted(funcs)
    for i, start in enumerate(starts):
        f = funcs[start]
        nxt = starts[i + 1] if i + 1 < len(starts) else None
        sec = elf.section_of(start)
        limit = sec["addr"] + sec["size"] if sec else start + 4
        if f["end"] is None or f["end"] <= start:
            f["end"] = min(nxt or limit, limit, start + 0x400) if f["kind"] == "stub" else min(nxt or limit, limit)
        f["name"] = names.get(start) or (f["gen"] if not f["gen"].startswith(("sub_", "FUN_")) else "")
        f["section"] = sec["name"] if sec else "?"
    return [funcs[s] for s in starts], source


def strip_code(text):
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', text)


def bodies(text):
    """Every brace-delimited function body in a C++ file, by function name (later definitions append)."""
    out = defaultdict(list)
    code = strip_code(text)
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(([^;{}()]|\([^()]*\))*\)\s*(?:const\s*)?(?:noexcept\s*)?\{", code):
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "catch", "return", "sizeof"):
            continue
        depth, i = 0, m.end() - 1
        while i < len(code):
            c = code[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        out[name].append(code[m.end():i])
    return out


class HandlerIndex:
    """PS2Recomp's stub and syscall handlers, classified by what their bodies (and the helpers they call) touch."""

    def __init__(self, ps2recomp):
        self.root = Path(ps2recomp) / "ps2xRuntime" / "src" / "lib" / "Kernel"
        self.defs = defaultdict(list)
        self.files = defaultdict(set)
        self.available = self.root.is_dir()
        if self.available:
            for path in sorted(self.root.rglob("*")):
                if path.suffix in (".cpp", ".h"):
                    for name, bs in bodies(read_text(path)).items():
                        self.defs[name].extend(bs)
                        self.files[name].add(path.relative_to(self.root).as_posix())
        self.cache = {}

    def classify(self, name):
        if name in self.cache:
            return self.cache[name]
        if not self.available or name not in self.defs:
            result = {"backing": "unknown", "evidence": "handler source not found", "file": ""}
            self.cache[name] = result
            return result
        text = "\n".join(self.defs[name])
        called = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", text)) - {name}
        helpers = [h for h in called if h in self.defs and len(self.defs[h]) <= 3]
        full = text + "\n" + "\n".join(b for h in helpers for b in self.defs[h])
        hits = [label for label, rx in EMU_KEYWORDS if re.search(rx, full)]
        weak = [label for label, rx in WEAK_KEYWORDS if re.search(rx, full)]
        result = {
            "backing": "emulation" if hits else "emulation?" if weak else "native",
            "evidence": ", ".join(hits + weak) if hits or weak else "no emulated subsystem referenced",
            "file": ", ".join(sorted(self.files[name])),
        }
        self.cache[name] = result
        return result


def syscall_table(ps2recomp):
    path = Path(ps2recomp) / "ps2xRuntime" / "src" / "lib" / "Kernel" / "Syscalls" / "Dispatcher.cpp"
    table, pending = {}, []
    for line in strip_code(read_text(path)).splitlines():
        cases = re.findall(r"case\s+(0x[0-9A-Fa-f]+|\d+)\s*:", line)
        if cases:
            pending.extend(int(c, 0) for c in cases)
        call = re.search(r"\b([A-Za-z_]\w*)\s*\(\s*rdram\s*,\s*ctx\s*,\s*runtime\s*\)", line)
        if call and pending:
            for n in pending:
                table[n] = call.group(1)
            pending = []
        if "return" in line and pending and not call:
            for n in pending:
                table.setdefault(n, f"inline 0x{n:02x}")
            pending = []
    return table


def env_gate(cond):
    env = re.match(r"(?:\}\s*)?(?:else )?if \((?:const char \*\s*\w+ = )?std::getenv\(\"(\w+)\"\)(.*)", cond)
    if env and not re.match(r"\s*;\s*!", env.group(2)):
        return env.group(1)
    return None


def load_overrides(sources, patterns, wrapper_patterns, base):
    """Override registrations in the port's runtime sources that are on by default, with the handler they install.
    Returns (overrides by address, registrations whose target could not be resolved to an address)."""
    consts, arrays, defs, regs, unresolved = {}, {}, {}, [], []
    texts = {}
    for path in sources:
        if not path.is_file():
            continue
        text = read_text(path)
        try:
            label = path.resolve().relative_to(base).as_posix()
        except ValueError:
            label = path.as_posix()
        texts[label] = text
        code = strip_code(text)
        for m in re.finditer(r"constexpr\s+(?:std::)?(?:uint32_t|u32|unsigned(?: int)?)\s+([^;\[]*);", code):
            for name, value in re.findall(r"\b(k\w+)\s*=\s*0x([0-9A-Fa-f]+)[uU]?\b", m.group(1)):
                consts.setdefault(name, int(value, 16))
        for m in re.finditer(r"constexpr\s+(?:std::)?(?:uint32_t|u32)\s+(k\w+)\[\w*\]\s*=\s*\{(.*?)\};", text, re.S):
            arrays[m.group(1)] = [(int(a, 16), n) for a, n in
                                  re.findall(r"0x([0-9A-Fa-f]+)[uU]?\s*,?\s*(?://\s*(\w+))?", m.group(2))]
        for name, bs in bodies(text).items():
            defs.setdefault(name, bs)
    regexes = [re.compile(p) for p in patterns]
    wrappers = [re.compile(p) for p in wrapper_patterns]
    for fname, text in texts.items():
        lines = text.splitlines()
        for i, line in enumerate(lines):
            if line.lstrip().startswith("//"):
                continue
            m = next((m for rx in regexes for m in [rx.search(line)] if m), None)
            if not m:
                continue
            if env_gate(line.strip()):
                continue
            indent = len(line) - len(line.lstrip())
            gated = None
            for j in range(i - 1, max(-1, i - 40), -1):
                prev = lines[j]
                pin = len(prev) - len(prev.lstrip())
                stripped = prev.strip()
                if not stripped or stripped in ("{", "}") or stripped.startswith("//"):
                    continue
                if pin < indent:
                    if stripped.startswith("for "):
                        indent = pin
                        continue
                    if stripped.startswith(("if ", "else if ", "} else if ")):
                        gated = env_gate(stripped)
                        if gated:
                            break
                        indent = pin
                        continue
                    if re.match(r"(void|bool|static|namespace)\b", stripped):
                        break
                    indent = pin
            if gated:
                continue
            target = m.group("target").strip()
            handler = re.sub(r"\s+", "", m.group("handler"))
            handler = "lambda" if handler == "[" else handler
            addrs = []
            literal = re.fullmatch(r"(?:static_cast<uint32_t>\()?0x([0-9A-Fa-f]+)[uU]?\)?", target)
            if literal:
                addrs = [(int(literal.group(1), 16), "")]
            elif target in consts:
                addrs = [(consts[target], target[1:])]
            else:
                loop = "\n".join(lines[max(0, i - 3):i])
                arr = re.search(r"for \((?:const )?(?:uint32_t|auto)\s*&?\s*(\w+)\s*:\s*(k\w+)\)", loop)
                if arr and arr.group(1) == target:
                    addrs = [(a, n or arr.group(2)[1:]) for a, n in arrays.get(arr.group(2), [])]
            if not addrs:
                unresolved.append({"file": fname, "line": i + 1, "target": target, "handler": handler})
                continue
            base_name = re.sub(r"<.*", "", handler.lstrip("&")).split("::")[-1]
            body = "\n".join(defs.get(base_name, []))
            wrapped = any(rx.search(body) for rx in wrappers)
            for addr, label in addrs:
                own = re.compile(rf"_0x0*{addr:x}\b", re.I)
                wrapper = wrapped or bool(own.search(handler)) or bool(re.search(own.pattern + r"\s*\(", body, re.I))
                regs.append({"addr": addr, "label": label[0].lower() + label[1:] if label else "", "handler": handler,
                             "file": fname, "wrapper": wrapper})
    out = {}
    for r in regs:
        out.setdefault(r["addr"], r)
    return out, unresolved


def condition_holds(cfg, cond, ps2recomp):
    """{"file": path, "match": regex} or {"file": path, "exists": true}, optionally "not": true. A path that starts
    with "ps2recomp:" is inside the PS2Recomp checkout."""
    path = str(cond.get("file", ""))
    target = cfg.resolve(path[len("ps2recomp:"):], Path(ps2recomp)) if path.startswith("ps2recomp:") else cfg.resolve(path)
    if "match" in cond:
        ok = re.search(cond["match"], read_text(target), re.M) is not None
    else:
        ok = target.exists()
    return not ok if cond.get("not") else ok


def load_vu_programs(cfg, ps2recomp):
    """Native VU microprogram entries: a lift spec ({"programs": {key: {unit, entries, pairs, file}}}) and/or a list
    of programs given in the config, merged by program key."""
    spec_cfg = cfg.get("vu_programs") or {}
    programs = {}
    spec_path = cfg.resolve(spec_cfg["lift_spec"]) if spec_cfg.get("lift_spec") else None
    lift_how = spec_cfg.get("lift_how", "lifted to C++ from the microcode ({file}); keeps the microcode's VU register model")
    if spec_path:
        text = read_text(spec_path)
        for key, p in (json.loads(text).get("programs", {}) if text else {}).items():
            programs[key] = {"unit": p.get("unit", ""), "pairs": p.get("pairs", 0),
                             "entries": {int(e, 16): lift_how.format(file=p.get("file", ""), program=key)
                                         for e in p.get("entries", [])}}
    for p in spec_cfg.get("programs", []):
        if not all(condition_holds(cfg, c, ps2recomp) for c in p.get("require", [])):
            continue
        key = str(p.get("program", "?"))
        slot = programs.setdefault(key, {"unit": p.get("unit", ""), "pairs": p.get("pairs", 0), "entries": {}})
        slot["pairs"] = slot["pairs"] or p.get("pairs", 0)
        slot["unit"] = slot["unit"] or p.get("unit", "")
        for e in p.get("entries", []):
            slot["entries"][int(str(e), 16)] = p.get("how", "native C++ port")
    out = []
    for key, p in programs.items():
        unit = "VU0" if str(p["unit"]).lower() == "vu0" or key.upper() == "VU0" else "VU1"
        entries = sorted(p["entries"])
        for i, e in enumerate(entries):
            nxt = entries[i + 1] if i + 1 < len(entries) else e + 8 * max(1, p["pairs"] - e // 8)
            out.append({"program": key, "unit": unit, "entry": e, "size": max(8, nxt - e), "how": p["entries"][e]})
    return out


def phys(addr):
    if 0xB0000000 <= addr < 0xB3000000:
        return addr & 0x1FFFFFFF
    return addr


def hw_region(addr):
    a = phys(addr)
    for lo, hi, kind, label in HW_REGIONS:
        if lo <= a < hi:
            return kind, label, a
    return None


def describe_hw(addr, load):
    region = hw_region(addr)
    if not region:
        return None
    kind, label, a = region
    detail = label
    if kind == "dma":
        base = max(c for c, _ in DMA_CHANNELS if c <= a)
        name = dict(DMA_CHANNELS)[base]
        reg = DMA_REGS.get(a - base, f"+0x{a - base:x}")
        detail = f"{name} {reg}"
        if reg == "CHCR":
            detail += " (poll)" if load else " (kick)"
    elif a in POLL_REGS:
        detail = POLL_REGS[a] + (" (poll)" if load else "")
    return kind, f"{'read' if load else 'write'} {detail} @0x{a:08x}"


LOADS = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x37, 0x1E, 0x1A, 0x1B, 0x31, 0x35, 0x36}
STORES = {0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x3F, 0x1F, 0x2C, 0x2D, 0x39, 0x3D, 0x3E}
GPR_LOADS = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x37, 0x1E, 0x1A, 0x1B}
CALL_CLOBBER = set(range(1, 16)) | {24, 25, 31}


def tag_like(value):
    top = value >> 24
    if top in (0x14, 0x15, 0x17) and value & 0x00FF0000 == 0:
        return f"VIF MSCAL-family code 0x{value:08x}"
    if top in (0x50, 0x51) and value & 0x00FF0000 == 0 and value & 0xFFFF:
        return f"VIF DIRECT code 0x{value:08x}"
    if 0x60 <= top <= 0x7F and top != 0x70 and value & 0x0000FFFF and not value & 0x3C00 and value & 0x00FF0000:
        return f"VIF UNPACK code 0x{value:08x}"
    return None


def analyze(words, start, mmio, starts):
    """Static scan of one function's MIPS: hardware accesses, VU control, calls, syscalls."""
    regs = {0: 0}
    touches = []
    materialized = {}
    deref = set()
    callees, tails = set(), set()
    syscalls = []
    jalr = 0
    pending_clobber = 0
    for i, w in enumerate(words):
        pc = start + i * 4
        op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        funct = w & 0x3F
        written = None
        value = None
        if op == 0x0F:
            written, value = rt, (imm << 16) & 0xFFFFFFFF
        elif op in (0x09, 0x08, 0x19, 0x18):
            written = rt
            if rs in regs:
                value = (regs[rs] + simm) & 0xFFFFFFFF
        elif op == 0x0D:
            written = rt
            if rs in regs:
                value = regs[rs] | imm
        elif op in (0x0C, 0x0E, 0x0A, 0x0B, 0x1C, 0x1D):
            written = rt
        elif op == 0x00:
            if funct in (0x21, 0x25, 0x2D, 0x20, 0x2C) and rd:
                written = rd
                if rt == 0 and rs in regs:
                    value = regs[rs]
                elif rs == 0 and rt in regs:
                    value = regs[rt]
            elif funct == 0x09:
                jalr += 1
                pending_clobber = 2
                written = rd
            elif funct == 0x08:
                if rs != 31:
                    jalr += 1
            elif funct == 0x0C:
                number = regs.get(3)
                syscalls.append((pc, None if number is None else (number if number < 0x80000000 else number - 0x100000000)))
            elif funct in (0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x11, 0x13):
                pass
            elif rd:
                written = rd
        elif op == 0x03:
            callees.add(((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
            pending_clobber = 2
        elif op == 0x02:
            target = ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if target in starts and target != start:
                tails.add(target)
        elif op == 0x12:
            if rs & 0x10:
                if funct in (0x38, 0x39):
                    touches.append(("vu0micro", True, pc, ("VCALLMS" if funct == 0x38 else "VCALLMSR") +
                                    (f" 0x{((w >> 6) & 0x7FFF) * 8:x}" if funct == 0x38 else " (CMSAR0)")))
            elif rs == 0x02 and rd in VU_CONTROL:
                touches.append(("vuctl", True, pc, f"CFC2 {VU_CONTROL[rd]}" + (" (poll)" if rd == 29 else "")))
                written = rt
            elif rs == 0x06 and rd in VU_CONTROL:
                what = {31: " (starts VU1)", 28: " (VU reset)", 27: " (VU0 start address)"}.get(rd, "")
                touches.append(("vuctl", True, pc, f"CTC2 {VU_CONTROL[rd]}{what}"))
            elif rs in (0x01, 0x02):
                written = rt
            elif rs == 0x08:
                touches.append(("vu0micro", True, pc, "BC2x (waits on VU0)"))
        elif op in (0x10, 0x11):
            if rs in (0x00, 0x01, 0x02):
                written = rt
        if op in LOADS or op in STORES:
            load = op in LOADS
            hit = describe_hw((regs[rs] + simm) & 0xFFFFFFFF, load) if rs in regs else None
            if hit:
                touches.append((hit[0], True, pc, hit[1]))
                deref.add(regs[rs])
            elif pc in mmio:
                hit = describe_hw(mmio[pc], load)
                if hit:
                    touches.append(("mmio", True, pc, hit[1] + " (recompiler config [mmio])"))
            if not load and rt in regs and rt:
                tag = tag_like(regs[rt])
                if tag:
                    touches.append(("tag", False, pc, f"stores {tag}"))
            if op in GPR_LOADS:
                written = rt
        if written:
            if value is None:
                regs.pop(written, None)
            else:
                regs[written] = value
                if hw_region(value) and op in (0x0F, 0x0D, 0x09, 0x19):
                    materialized.setdefault(value & 0xFFFF0000, (pc, value))
        if pending_clobber:
            pending_clobber -= 1
            if pending_clobber == 0:
                for r in CALL_CLOBBER:
                    regs.pop(r, None)
    used = {v & 0xFFFF0000 for v in deref}
    for high, (pc, value) in materialized.items():
        if value & 0xFFFF == 0 and hw_region(value)[0] in ("timer", "eeio", "vumem"):
            continue
        if high not in used and not any(t[0] not in ("addr", "tag", "vu0micro", "vuctl") for t in touches):
            region = hw_region(value)
            touches.append(("addr", False, pc, f"builds {region[1]} address 0x{phys(value):08x} without using it here"))
    return {"touches": touches, "callees": callees | tails, "syscalls": syscalls, "jalr": jalr}


GAME_CHUNK = 0x80000


def assign_libraries(funcs, libraries, game_code_end, unnamed_label):
    named = [(i, f) for i, f in enumerate(funcs) if f["name"]]
    lib_of = {}
    for i, f in named:
        if f.get("syscall"):
            lib_of[i] = "libkernel"
            continue
        for label, rx in libraries:
            if re.search(rx, f["name"]):
                lib_of[i] = label
                break
    region = ["" for _ in funcs]
    keys = sorted(lib_of)
    runs = []
    for k in keys:
        if runs and lib_of[runs[-1][-1]] == lib_of[k]:
            runs[-1].append(k)
        else:
            runs.append([k])
    keys = [k for run in runs if len(run) > 1 for k in run]
    for a, b in zip(keys, keys[1:] + [None]):
        region[a] = lib_of[a]
        if b is not None and lib_of[a] == lib_of[b] and funcs[b]["addr"] - funcs[a]["addr"] < 0x20000:
            for k in range(a + 1, b):
                region[k] = lib_of[a]
    first_lib = keys[0] if keys else len(funcs)
    big = [k for k in keys if funcs[k]["addr"] >= game_code_end] if game_code_end is not None else []
    game_end = big[0] if big else first_lib
    for k in range(len(funcs)):
        if not region[k]:
            region[k] = "game code" if k < game_end else unnamed_label
    for k, f in enumerate(funcs):
        if region[k] == "game code":
            lo = f["addr"] & ~(GAME_CHUNK - 1)
            region[k] = f"game code {hx(lo)}-{hx(lo + GAME_CHUNK - 1)}"
    return region


def irx_modules(generated):
    out = []
    iop = Path(generated) / "iop" if generated else None
    if not iop or not iop.is_dir():
        return out
    for path in sorted(iop.glob("*_iop.cpp")):
        text = read_text(path)
        reg = re.search(r'Registration kModule\("([^"]+)",\s*0x([0-9a-f]+)ull,\s*0x([0-9a-f]+)u,\s*0x([0-9a-f]+)u', text)
        if not reg:
            continue
        name = reg.group(1).split(".")[0]
        base, size = int(reg.group(3), 16), int(reg.group(4), 16)
        defs = list(re.finditer(r"^void (iop_[0-9a-f]+_([0-9a-f]+))\(Env &e\)\s*\{", text, re.M))
        funcs = []
        for k, m in enumerate(defs):
            body = text[m.end():defs[k + 1].start() if k + 1 < len(defs) else len(text)]
            addr = int(m.group(2), 16)
            interp = re.findall(r"// (0x[0-9a-f]{8}): the interpreter runs it", body)
            funcs.append({"addr": addr, "interp": len(interp), "words": sorted(set(interp))[:4]})
        funcs.sort(key=lambda f: f["addr"])
        for k, f in enumerate(funcs):
            f["size"] = (funcs[k + 1]["addr"] if k + 1 < len(funcs) else base + size) - f["addr"]
        out.append({"module": name, "base": base, "size": size, "funcs": funcs})
    return out


def irx_lists(build):
    """tools/iop_recomp.py --list outputs (build/iop_<MODULE>_list.txt from scripts/iop_recomp.sh), if present."""
    out = {}
    for path in sorted(build.glob("iop_*_list.txt")) if build and build.is_dir() else []:
        module = path.stem[4:-5].upper()
        for m in re.finditer(r"function (0x[0-9a-f]+): (\d+) instructions, interpreted \(([^)]*)\)", read_text(path)):
            out.setdefault(module, []).append((int(m.group(1), 16), int(m.group(2)) * 4, m.group(3)))
    return out


def iop_hle_services(ps2recomp):
    names = {}
    mods = Path(ps2recomp) / "ps2xIOP" / "src" / "modules"
    for path in sorted(mods.glob("*.cpp")) if mods.is_dir() else []:
        text = read_text(path)
        aliases = re.findall(r'"([A-Za-z0-9_]+)"', " ".join(re.findall(r"m?_?[mM]oduleAliases\s*=\s*\{([^}]*)\}", text)))
        aliases += re.findall(r'name\(\) const override\s*\{\s*return "([^"]+)"', text)
        for a in aliases or [path.stem]:
            names[a.upper()] = path.name
    return names


def runtime_rows(cfg, ps2recomp):
    """The config's runtime_badges: each {"name", "rules": [{"if": [conditions], "status", "evidence"}, ...]}; the
    first rule whose conditions all hold gives the badge (a rule without "if" always holds)."""
    rows = []
    for badge in cfg.get("runtime_badges", []):
        for rule in badge.get("rules", []):
            if all(condition_holds(cfg, c, ps2recomp) for c in rule.get("if", [])):
                rows.append({"name": badge.get("name", "?"), "status": rule.get("status", "present"),
                             "evidence": rule.get("evidence", "")})
                break
    return rows


def backing_text(c):
    if c["backing"] == "emulation":
        return "emulation-backed: " + c["evidence"]
    if c["backing"] == "emulation?":
        return "possibly emulation-backed (uncertain): " + c["evidence"]
    if c["backing"] == "native":
        return "native: " + c["evidence"]
    return c["evidence"]


def backing_flags(c):
    if c["backing"] == "native":
        return 0
    return FLAG_EMU_STUB | (FLAG_UNCERTAIN if c["backing"] != "emulation" else 0)


def load_coverage(path):
    """Guest PCs from a PS2X_COVERAGE file (or any text with one hex address per line; # starts a comment)."""
    pcs = set()
    for line in read_text(path).splitlines():
        line = line.split("#", 1)[0].strip()
        m = re.fullmatch(r"(?:0x)?([0-9a-fA-F]{1,8})", line)
        if m:
            pcs.add(int(m.group(1), 16) & 0x1FFFFFFF)
    return pcs


def ran_functions(funcs, pcs):
    """Functions whose range holds a recorded PC: the dispatcher also records resume points inside a function."""
    starts = [f["addr"] for f in funcs]
    ran = set()
    for pc in pcs:
        k = bisect.bisect_right(starts, pc) - 1
        if k >= 0 and pc < max(funcs[k]["end"], funcs[k]["addr"] + 4):
            ran.add(funcs[k]["addr"])
    return ran


def classify(funcs, elf, config, handlers, sysnames, overrides, coverage):
    starts = {f["addr"] for f in funcs}
    by_addr = {f["addr"]: f for f in funcs}
    for f in funcs:
        f["flags"] = 0
        f["calls"] = []
        if coverage is not None and f["addr"] in coverage:
            f["flags"] |= FLAG_RAN
        if f["addr"] in overrides:
            o = overrides[f["addr"]]
            f["state"] = NATIVE
            f["name"] = f["name"] or o["label"]
            f["reason"] = f"game override {o['handler']} ({o['file']})" + \
                ("; wraps the original, which still runs on some paths" if o["wrapper"] else "")
            if o["wrapper"]:
                f["flags"] |= FLAG_WRAPPER
            continue
        if f["kind"] == "stub":
            c = handlers.classify(f["handler"])
            f["state"] = STUB
            f["name"] = f["name"] or f["handler"]
            f["backing"] = c["backing"]
            f["reason"] = f"{f['ns']}::{f['handler']} ({c['file'] or 'source not found'}): " + backing_text(c)
            f["flags"] |= backing_flags(c)
            continue
        words = elf.words(f["addr"], f["end"])
        a = analyze(words, f["addr"], config["mmio"], starts)
        f["analysis"] = a
        f["calls"] = sorted(c for c in a["callees"] if c in starts and c != f["addr"])
        if a["syscalls"] and len(words) <= 8 and all(n is not None for _, n in a["syscalls"]):
            number = a["syscalls"][0][1]
            name = sysnames.get(abs(number), f"syscall 0x{abs(number):02x}")
            c = handlers.classify(name) if not name.startswith(("inline", "syscall")) else \
                {"backing": "native", "evidence": "handled inline by the dispatcher", "file": "Syscalls/Dispatcher.cpp"}
            f["state"] = STUB
            f["name"] = f["name"] if f["name"] and not f["name"].startswith("entry_") else \
                name if not name.startswith(("inline", "syscall")) else f"syscall {number:#x}"
            f["syscall"] = True
            f["backing"] = c["backing"]
            f["reason"] = f"kernel syscall {number:#x} -> ps2_syscalls::{name} ({c['file']}): " + backing_text(c)
            f["flags"] |= backing_flags(c)
            continue
        certain = [t for t in a["touches"] if t[1]]
        unsure = [t for t in a["touches"] if not t[1]]
        if certain or unsure:
            f["state"] = DIRECT
            picked = (certain or unsure)
            kinds = Counter(t[0] for t in picked)
            f["kinds"] = sorted(kinds)
            f["reason"] = "; ".join(f"{t[3]} at 0x{t[2]:06x}" for t in picked[:4]) + \
                (f"; +{len(picked) - 4} more" if len(picked) > 4 else "")
            if not certain:
                f["flags"] |= FLAG_UNCERTAIN
                f["reason"] = "uncertain: " + f["reason"]
        else:
            f["state"] = CLEAN
            f["reason"] = "no hardware access, VU control or emulation-backed call found"
        if a["jalr"]:
            f["reason"] += f"; {a['jalr']} indirect call(s)/jump(s) not followed"
    touch = {f["addr"] for f in funcs if (f["state"] == DIRECT or (f["state"] == STUB and f["flags"] & FLAG_EMU_STUB))
             and not f["flags"] & FLAG_UNCERTAIN}
    depth1 = {}
    for f in funcs:
        if f["state"] != CLEAN:
            continue
        hit = [c for c in f["calls"] if c in touch]
        if hit:
            depth1[f["addr"]] = hit
    depth2 = {}
    for f in funcs:
        if f["state"] != CLEAN or f["addr"] in depth1:
            continue
        hit = [c for c in f["calls"] if c in depth1]
        if hit:
            depth2[f["addr"]] = hit

    def via(addr, seen=()):
        g = by_addr[addr]
        if g["state"] == STUB:
            return {"stub"}
        if g["state"] == DIRECT:
            return {"vu0" if set(g.get("kinds", [])) <= {"vu0micro"} else "hardware"}
        out = set()
        for c in depth1.get(addr, []):
            out |= via(c)
        return out

    def label(addr):
        g = by_addr[addr]
        return f"{g['name'] or 'sub'}@{hx(addr)}"

    for f in funcs:
        if f["addr"] in depth1:
            hit = depth1[f["addr"]]
            f["state"] = INDIRECT
            f["depth"] = 1
            f["reason"] = "calls " + ", ".join(
                label(c) + (" (emulation-backed stub)" if by_addr[c]["state"] == STUB else "") for c in hit[:3]) + \
                (f" +{len(hit) - 3} more" if len(hit) > 3 else "")
        elif f["addr"] in depth2:
            hit = depth2[f["addr"]]
            f["state"] = INDIRECT
            f["depth"] = 2
            f["reason"] = "calls " + ", ".join(f"{label(c)} -> {label(depth1[c][0])}" for c in hit[:2]) + \
                (f" +{len(hit) - 2} more" if len(hit) > 2 else "")
        if f["state"] == INDIRECT:
            f["via"] = sorted(set().union(*(via(c) for c in (depth1.get(f["addr"]) or depth2[f["addr"]]))))
    return funcs


def svg_map(funcs, counts, meta, colors):
    pitch, cell, cols = 5, 4, 160
    top = 80
    rows = (len(funcs) + cols - 1) // cols
    width, height = cols * pitch + 16, top + rows * pitch + 12
    order = ["native", "clean", "direct", "indirect", "stub", "stubemu"]
    css = [":root{--bg:#fbfaf7;--fg:#24211c;--muted:#6b665d}"]
    dark = []
    for key in order:
        css.append(f".{key}{{fill:{colors['light'][key]}}}")
        dark.append(f".{key}{{fill:{colors['dark'][key]}}}")
    style = " ".join(css) + " text{fill:var(--fg);font:12px system-ui,sans-serif} .m{fill:var(--muted)} " \
        "rect.bg{fill:var(--bg)} .stripe{stroke:var(--bg);stroke-width:1} " \
        "@media (prefers-color-scheme: dark){:root{--bg:#1b1a17;--fg:#ece8df;--muted:#a39d91} " + " ".join(dark) + "}"
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
           f'role="img" aria-label="{html.escape(meta["title"])}">', f"<style>{style}</style>",
           f'<rect class="bg" width="{width}" height="{height}"/>', "<defs>"]
    for key in order:
        out.append(f'<pattern id="p-{key}" width="{pitch}" height="{pitch}" patternUnits="userSpaceOnUse">'
                   f'<rect class="{key}" width="{cell}" height="{cell}"/>' +
                   (f'<path class="stripe" d="M0 {cell} L{cell} 0"/>' if key == "direct" else "") +
                   (f'<rect class="bg" x="1.5" y="1.5" width="1" height="1"/>' if key == "stubemu" else "") +
                   "</pattern>")
    out.append("</defs>")
    pct = meta["emulation_free_pct"]
    out.append(f'<text x="8" y="18" style="font-weight:600;font-size:14px">{html.escape(meta["title"])}: '
               f'{counts["direct"] + counts["indirect"]} of {counts["total"]} functions still touch emulation '
               f'({pct:.1f}% emulation-free)</text>')
    labels = [("native", "native C++"), ("clean", "recompiled, clean"), ("direct", "touches emulation"),
              ("indirect", "calls emulation"), ("stub", "native stub"), ("stubemu", "emulation-backed stub")]
    for k, (key, text) in enumerate(labels):
        n = counts.get(key, 0) if key not in ("stub", "stubemu") else counts["stub_native" if key == "stub" else "stub_emulation"]
        x, y = 8 + (k % 3) * 260, 28 + (k // 3) * 16
        out.append(f'<rect class="{key}" x="{x}" y="{y}" width="10" height="10"/>' +
                   (f'<path class="stripe" d="M{x} {y + 10} L{x + 10} {y}"/>' if key == "direct" else "") +
                   (f'<rect class="bg" x="{x + 4}" y="{y + 4}" width="2" height="2"/>' if key == "stubemu" else "") +
                   f'<text x="{x + 15}" y="{y + 9}" class="m">{text} {n}</text>')
    out.append(f'<text x="8" y="{top - 8}" class="m">{html.escape(meta["subtitle"])}</text>')

    def key_of(f):
        if f["state"] == STUB:
            return "stubemu" if f["flags"] & FLAG_EMU_STUB else "stub"
        return STATES[f["state"]][0]

    for r in range(rows):
        run_start, run_key = 0, None
        row = funcs[r * cols:(r + 1) * cols]
        for c, f in enumerate(row + [None]):
            k = key_of(f) if f else None
            if k != run_key:
                if run_key is not None:
                    out.append(f'<rect x="{8 + run_start * pitch}" y="{top + r * pitch}" width="{(c - run_start) * pitch}" '
                               f'height="{pitch}" fill="url(#p-{run_key})"/>')
                run_start, run_key = c, k
    out.append("</svg>")
    return "\n".join(out)


COLORS = {
    "light": {"native": "#0868ac", "clean": "#7fbfa6", "direct": "#d1495b", "indirect": "#f2a7a0",
              "stub": "#9c7fc1", "stubemu": "#6a3d9a"},
    "dark": {"native": "#56a8e8", "clean": "#2f7a62", "direct": "#ff6b6b", "indirect": "#8f4a48",
             "stub": "#a48bd0", "stubemu": "#c9b3f0"},
}


def write_outputs(out_dir, history_path, payload, funcs):
    out_dir.mkdir(parents=True, exist_ok=True)
    counts = payload["counts"]
    history = []
    if history_path.exists():
        try:
            history = json.loads(history_path.read_text(encoding="utf-8"))
        except ValueError:
            history = []
    point = {"date": payload["meta"]["generated"], "port": payload["meta"]["port_commit"],
             "ps2recomp": payload["meta"]["ps2recomp_commit"], "tool": TOOL_VERSION,
             "counts": {k: v for k, v in counts.items() if k != "ran"}}
    same = lambda h: (h.get("port", h.get("sc3")), h.get("ps2recomp"), h.get("tool"), h.get("counts")) == \
        (point["port"], point["ps2recomp"], point["tool"], point["counts"])
    if history and same(history[-1]):
        history[-1]["date"] = point["date"]
    else:
        history.append(point)
    history_path.write_text(json.dumps(history, indent=1) + "\n", encoding="utf-8")
    payload["history"] = history
    (out_dir / "map.json").write_text(json.dumps(payload, indent=1) + "\n", encoding="utf-8")
    (out_dir / "map.svg").write_text(svg_map(funcs, counts, payload["meta"], COLORS) + "\n", encoding="utf-8")
    (out_dir / "map.html").write_text(render_html(payload), encoding="utf-8")


def compact(payload):
    strings, index = [], {}

    def s(text):
        if text not in index:
            index[text] = len(strings)
            strings.append(text)
        return index[text]

    position = {(f["addr"], f["region"].startswith("VU ")): i for i, f in enumerate(payload["functions"])}
    funcs = []
    for f in payload["functions"]:
        calls = [position[(int(c, 16), False)] for c in f["calls"][:8] if (int(c, 16), False) in position]
        via = sum(1 << ("hardware", "vu0", "stub").index(v) for v in f["via"])
        funcs.append([f["addr"], f["size"], f["state"], s(f["name"]), s(f["reason"]), f["flags"], s(f["region"]),
                      f["depth"], via, calls])
    irx = []
    for m in payload["irx"]:
        irx.append({"module": m["module"], "base": m["base"], "size": m["size"],
                    "funcs": [[g["addr"], g["size"], g["state"], s(g["reason"])] for g in m["functions"]]})
    return {"meta": payload["meta"], "counts": payload["counts"], "runtime": payload["runtime"],
            "heuristics": payload["heuristics"], "irx_counts": payload["irx_counts"], "kinds": payload["kinds"],
            "history": payload["history"], "strings": strings, "funcs": funcs, "irx": irx,
            "states": [{"id": k, "label": l, "desc": d} for k, l, d in STATES]}


def render_html(payload):
    data = json.dumps(compact(payload), separators=(",", ":")).replace("</", "<\\/")
    page = HTML_TEMPLATE.replace("/*DATA*/null", data)
    page = page.replace("{{TITLE}}", html.escape(payload["meta"]["page_title"]))
    return page


def resolve_inputs(cfg, args):
    elf = cfg.path_of("elf", args.elf)
    recomp = cfg.path_of("recomp_config")
    generated = cfg.path_of("generated", args.generated)
    if generated and not generated.is_dir():
        generated = None
    ps2recomp = cfg.path_of("ps2recomp", args.ps2recomp) or FORK_ROOT
    csv = cfg.path_of("function_list_csv")
    sources = cfg.paths_of("runtime_sources")
    irx = cfg.get("irx") or {}
    lists = cfg.resolve(irx["lists_dir"]) if irx.get("lists_dir") else None
    port_root = cfg.path_of("port_root") or Path(git(cfg.base, "rev-parse", "--show-toplevel") or cfg.base)
    return {"elf": elf, "recomp_config": recomp, "generated": generated, "ps2recomp": Path(ps2recomp),
            "function_list_csv": csv, "runtime_sources": sources, "irx_lists": lists, "port_root": port_root}


def check(cfg, inputs):
    """--check: what the config resolves to and what each input gives, without the ELF scan."""
    ok = True
    for key in ("elf", "recomp_config", "generated", "ps2recomp", "function_list_csv", "irx_lists", "port_root"):
        value = inputs[key]
        print(f"{key:18} {value if value else '(not found)'}")
    if not inputs["elf"]:
        ok = False
        print("  the ELF is required for a map", file=sys.stderr)
    if not inputs["generated"] and not inputs["function_list_csv"]:
        ok = False
        print("  neither a generated tree nor a function list CSV: no function list", file=sys.stderr)
    files = [p for p in inputs["runtime_sources"] if p.is_file()]
    print(f"runtime_sources    {len(files)} files")
    rc = load_recomp_config(inputs["recomp_config"])
    print(f"recompiler config  {len(rc['stubs'])} stubs, {len(rc['entries'])} entry points, {len(rc['mmio'])} [mmio]")
    overrides, unresolved = load_overrides(files, cfg.get("override_patterns", DEFAULT_OVERRIDE_PATTERNS),
                                           cfg.get("wrapper_patterns", DEFAULT_WRAPPER_PATTERNS), inputs["port_root"])
    wrappers = sum(1 for o in overrides.values() if o["wrapper"])
    print(f"overrides          {len(overrides)} addresses on by default ({wrappers} wrappers), "
          f"{len(unresolved)} registrations with a target not resolved to an address")
    for u in unresolved[:12]:
        print(f"  unresolved {u['file']}:{u['line']} {u['target']} -> {u['handler']}")
    vu = load_vu_programs(cfg, inputs["ps2recomp"])
    print(f"vu programs        {len(vu)} native entries")
    handlers = HandlerIndex(inputs["ps2recomp"])
    print(f"ps2recomp stubs    {len(handlers.defs)} handler bodies indexed" if handlers.available else
          "ps2recomp stubs    (no ps2xRuntime sources: stubs unclassified)")
    for row in runtime_rows(cfg, inputs["ps2recomp"]):
        print(f"badge              {row['name']}: {row['status']}")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--config", required=True, help="the port's JSON config (see docs/func_map.md)")
    ap.add_argument("--elf", help="game ELF (overrides the config's elf)")
    ap.add_argument("--generated", help="generated tree of ps2recomp (overrides the config's generated)")
    ap.add_argument("--ps2recomp", help="PS2Recomp checkout whose runtime answers the stubs (default: the config's, "
                    "then the checkout holding this tool)")
    ap.add_argument("--out", help="output directory (default: the config's out, then ./funcmap)")
    ap.add_argument("--history", help="history file to append to (default: <out>/history.json)")
    ap.add_argument("--coverage", help="PS2X_COVERAGE file of the guest PCs that ran (or one hex address per line)")
    ap.add_argument("--check", action="store_true", help="print what the config resolves to and exit")
    args = ap.parse_args()

    cfg = PortConfig(args.config)
    inputs = resolve_inputs(cfg, args)
    if args.check:
        sys.exit(check(cfg, inputs))
    if not inputs["elf"]:
        raise SystemExit(f"no game ELF: {cfg.get('elf')!r} not found (pass --elf)")
    generated = inputs["generated"]
    if not generated:
        print(f"no generated tree ({cfg.get('generated')!r}): falling back to the function list CSV", file=sys.stderr)
    ps2recomp = inputs["ps2recomp"]
    if not (ps2recomp / "ps2xRuntime").is_dir():
        print(f"no PS2Recomp sources at {ps2recomp} (--ps2recomp): stubs unclassified", file=sys.stderr)

    elf = Elf(inputs["elf"])
    config = load_recomp_config(inputs["recomp_config"])
    funcs, source = load_functions(generated, config, elf, inputs["function_list_csv"])
    handlers = HandlerIndex(ps2recomp)
    sysnames = syscall_table(ps2recomp)
    overrides, unresolved = load_overrides(inputs["runtime_sources"],
                                           cfg.get("override_patterns", DEFAULT_OVERRIDE_PATTERNS),
                                           cfg.get("wrapper_patterns", DEFAULT_WRAPPER_PATTERNS), inputs["port_root"])
    coverage = None
    pcs = set()
    if args.coverage:
        pcs = load_coverage(args.coverage)
        coverage = ran_functions(funcs, pcs)
    classify(funcs, elf, config, handlers, sysnames, overrides, coverage)
    libraries = [tuple(x) for x in cfg.get("libraries", [])] + LIBRARIES
    end = cfg.get("game_code_end")
    regions = assign_libraries(funcs, libraries, int(str(end), 16) if end is not None else None,
                               cfg.get("unnamed_library_label", "unnamed library code"))
    vu = load_vu_programs(cfg, ps2recomp)

    records = []
    for f, region in zip(funcs, regions):
        records.append({"addr": f["addr"], "size": f["end"] - f["addr"], "name": f["name"], "state": f["state"],
                        "reason": f["reason"], "flags": f["flags"], "region": f"{f['section']} / {region}",
                        "kinds": f.get("kinds", []), "calls": [hx(c) for c in f["calls"][:16]],
                        "depth": f.get("depth", 0), "backing": f.get("backing", ""), "via": f.get("via", [])})
    for p in vu:
        records.append({"addr": p["entry"], "size": p["size"], "name": f"{p['unit']} program {p['program']}:0x{p['entry']:x}",
                        "state": NATIVE, "reason": p["how"], "flags": 0,
                        "region": f"VU microprograms / {p['unit']} program {p['program']}", "kinds": [], "calls": [],
                        "depth": 0, "backing": "", "via": []})

    ee = [r for r in records if not r["region"].startswith("VU ")]
    counts = Counter(STATES[r["state"]][0] for r in ee)
    counts = {k: counts.get(k, 0) for k, _, _ in STATES}
    counts["total"] = len(ee)
    counts["uncertain"] = sum(1 for r in ee if r["state"] == DIRECT and r["flags"] & FLAG_UNCERTAIN)
    counts["stub_emulation"] = sum(1 for r in ee if r["state"] == STUB and r["flags"] & FLAG_EMU_STUB)
    counts["stub_native"] = counts["stub"] - counts["stub_emulation"]
    counts["indirect_depth1"] = sum(1 for r in ee if r["depth"] == 1)
    counts["vu_programs_native"] = len(vu)
    counts["indirect_via_vu0_only"] = sum(1 for r in ee if r["state"] == INDIRECT and r["via"] == ["vu0"])
    if coverage is not None:
        ran = Counter(STATES[r["state"]][0] for r in ee if r["flags"] & FLAG_RAN)
        counts["ran"] = {k: ran.get(k, 0) for k, _, _ in STATES}
        counts["ran"]["total"] = sum(ran.values())
        counts["ran"]["stub_emulation"] = sum(1 for r in ee if r["flags"] & FLAG_RAN and r["state"] == STUB
                                              and r["flags"] & FLAG_EMU_STUB)
        counts["ran"]["pcs"] = len(pcs)
    free = counts["native"] + counts["clean"] + counts["stub_native"]
    kinds = Counter(k for r in ee if r["state"] == DIRECT for k in r["kinds"])

    irx = irx_modules(generated)
    hle = iop_hle_services(ps2recomp)
    listed = irx_lists(inputs["irx_lists"])
    irx_counts = Counter()
    for m in irx:
        provider = hle.get(m["module"].upper(), "")
        m["hle_provider"] = provider
        m["functions"] = []
        seen = set()
        for g in m.pop("funcs"):
            seen.add(g["addr"])
            breaks = all((int(word, 16) & 0xFC00003F) == 0x0D for word in g["words"])
            if g["interp"] and not breaks:
                state, reason = 1, f"{g['interp']} instruction(s) left to the interpreter: " + ", ".join(g["words"])
            else:
                state = 0
                reason = "recompiled by tools/iop_recomp.py; runs on the emulated IOP kernel"
                if g["interp"]:
                    reason += f"; {g['interp']} BREAK trap(s) left to the interpreter (gcc's divide-by-zero check)"
            if provider:
                reason += f"; ps2xIOP {provider} exists only as a fallback when the IRX is missing"
            m["functions"].append({"addr": g["addr"], "size": g["size"], "state": state, "reason": reason})
        for addr, size, why in listed.get(m["module"].upper(), []):
            if addr not in seen:
                m["functions"].append({"addr": addr, "size": size, "state": 1,
                                       "reason": f"left to the interpreter by tools/iop_recomp.py ({why})"})
        m["functions"].sort(key=lambda g: g["addr"])
        for g in m["functions"]:
            irx_counts[("recompiled", "interpreted", "overridden")[g["state"]]] += 1
    port_root = inputs["port_root"]
    port_commit = git(port_root, "rev-parse", "--short=10", "HEAD")
    pinned = git(port_root, "ls-tree", "HEAD", cfg.get("ps2recomp_submodule", "PS2Recomp")).split()
    short = cfg.short()
    meta = {
        "game": cfg.get("game_id", ""), "title": cfg.title(), "short_name": short,
        "page_title": cfg.get("page_title", f"{short} Native Map"),
        "heading": cfg.get("heading", f"{cfg.get('title', short)} native map"),
        "generated": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "port_commit": port_commit, "port_branch": git(port_root, "rev-parse", "--abbrev-ref", "HEAD"),
        "ps2recomp_commit": pinned[2][:10] if len(pinned) > 2 else git(ps2recomp, "rev-parse", "--short=10", "HEAD"),
        "function_source": source, "tool_version": TOOL_VERSION,
        "emulation_free_pct": 100.0 * free / max(1, counts["total"]),
        "coverage": bool(coverage is not None),
        "coverage_file": Path(args.coverage).name if args.coverage else "",
        "unresolved_overrides": len(unresolved),
        "sections": [{"name": s["name"], "addr": s["addr"], "size": s["size"]} for s in elf.sections if s["exec"]],
    }
    meta["subtitle"] = f"one square per function, address order; {short} {port_commit or '?'}, PS2Recomp " \
                       f"{meta['ps2recomp_commit'] or '?'}, {meta['generated'][:10]}"
    heuristics = {"rules": HEURISTICS["rules"], "blind": HEURISTICS["blind"] + list(cfg.get("blind_spots", []))}
    payload = {"meta": meta, "counts": counts, "kinds": {TOUCH_KINDS.get(k, k): n for k, n in kinds.most_common()},
               "runtime": runtime_rows(cfg, ps2recomp), "heuristics": heuristics, "functions": records,
               "irx": irx, "irx_counts": {k: irx_counts.get(k, 0) for k in ("recompiled", "interpreted", "overridden")},
               "irx_hle_providers": hle}
    out_dir = Path(args.out) if args.out else (cfg.path_of("out", must_exist=False) or Path("funcmap"))
    history_path = Path(args.history) if args.history else out_dir / "history.json"
    write_outputs(out_dir, history_path, payload, [f for f in funcs])
    print(f"{counts['total']} EE functions: native {counts['native']}, clean {counts['clean']}, direct {counts['direct']} "
          f"({counts['uncertain']} uncertain), indirect {counts['indirect']}, stubs {counts['stub']} "
          f"({counts['stub_emulation']} emulation-backed); {len(vu)} VU program entries native; "
          f"IRX {dict(payload['irx_counts'])}")
    if unresolved:
        print(f"{len(unresolved)} override registration(s) with a target not resolved to an address (--check lists them)")
    if coverage is not None:
        r = counts["ran"]
        print(f"coverage: {len(pcs)} PCs, {r['total']} functions ran: native {r['native']}, clean {r['clean']}, "
              f"direct {r['direct']}, indirect {r['indirect']}, stubs {r['stub']} ({r['stub_emulation']} emulation-backed)")
    print(f"wrote {out_dir / 'map.html'}, map.svg, map.json, {history_path}")


HEURISTICS = {
    "rules": [
        ["Function list", "The function boundaries ps2_recomp used: the // Function / // Address header of every file "
         "in the generated tree. Files whose body only calls ps2_stubs:: or ps2_syscalls:: are the stubs. Names come "
         "from the recompiler config's stubs and entry_points and from the override constants. Without a generated "
         "tree the tool falls back to the config's function list CSV, with ends taken from the next start."],
        ["Native C++", "An override registration (registerFunction, replaceFunction, or the config's "
         "override_patterns) in the port's runtime sources that is not behind an environment-variable diagnostic "
         "(if (std::getenv(...)) blocks are skipped, 'unset or not 0' defaults are kept). Its target is a hex literal, "
         "a constexpr kName = 0x... constant, or the variable of a for loop over a constexpr array; other targets are "
         "counted as unresolved. Overrides that call the original back on some paths are flagged as wrappers. The VU "
         "microprogram entries come from the config's VU program specs."],
        ["Touches emulation (direct)", "A linear pass over the function's MIPS that tracks the constants built by "
         "LUI/ORI/ADDIU/moves and drops caller-saved registers after each call. A load or store whose address lands "
         "in EE I/O (0x10000000-0x1000FFFF: timers, IPU, GIF, VIF0/1, FIFOs, DMA channels, DMAC, INTC, SIF), VU memory "
         "(0x11000000-0x1100FFFF) or the GS privileged registers (0x12000000-0x12001FFF), also through KSEG1, counts. "
         "A CHCR read is a poll, a CHCR write a kick; D_STAT, GS CSR, I_STAT and SIF SMFLG reads are polls. VU: "
         "VCALLMS/VCALLMSR, BC2x and CFC2/CTC2 of CMSAR0, FBRST, VPU-STAT and CMSAR1. The recompiler config's [mmio] "
         "table (addresses the analyzer resolved) is checked as a second source."],
        ["Uncertain", "Shown hollow. A store of a constant shaped like a VIF code (MSCAL, MSCALF, MSCNT, DIRECT, "
         "DIRECTHL, UNPACK with a plausible NUM and ADDR), or a hardware register address built but not "
         "dereferenced in the function (passed to a callee or stored). Uncertain hits never propagate to callers."],
        ["Calls emulation (indirect)", "Depth 1: a JAL or tail J to a function that touches emulation for certain, "
         "or to a stub whose handler is emulation-backed for certain. Depth 2: a call to a depth-1 function. "
         "Nothing propagates through a native override. 'Via' says what is at the bottom: hardware access, VU0 micro "
         "mode only, or an emulation-backed stub."],
        ["Stub backing", "The handler's body in PS2Recomp's ps2xRuntime/src/lib/Kernel, plus the helpers it calls "
         "directly, is searched for references to an emulated subsystem: I/O register addresses, the DMAC, GIF, VIF, "
         "GS memory or registers, VU1, the IOP and SIF, the SPU2, the interrupt model. A handler that only waits on "
         "the emulated vblank is 'possibly emulation-backed' (uncertain)."],
        ["Syscall wrappers", "A function of at most 8 instructions with a SYSCALL whose $v1 is a known constant is "
         "the kernel's wrapper: it is classed as a stub of the handler PS2Recomp's Dispatcher.cpp maps that number "
         "to (negative numbers are the interrupt-context variants)."],
        ["IRX modules", "The functions in generated/iop/*_iop.cpp (tools/iop_recomp.py). BREAK traps (gcc's "
         "divide-by-zero check) left to the interpreter do not count as a fallback; any other instruction does, and "
         "so does a function the --list output marks as interpreted. A ps2xIOP HLE service with the module's name "
         "only answers when the game's IRX is missing."],
        ["Coverage", "With --coverage, a function ran when the PS2X_COVERAGE file holds a PC inside its range: the "
         "runtime records every guest PC its dispatcher looks up (calls, jumps, returns to a caller, interrupt and "
         "callback entries), so a function counts once something dispatched into it."],
    ],
    "blind": [
        "Indirect calls are not followed: JALR, jump tables, function pointers, callbacks and interrupt handlers. "
        "Each function's details give how many it has.",
        "The constant tracking ignores control flow and memory: a hardware address loaded from a table or a global "
        "(libdma keeps channel pointers in data) is missed, and a register reused on two paths can be wrong.",
        "GIF tags and DMA tags are 64- and 128-bit values built with DSLL32, OR and PCPYLD: they are not recognised. "
        "Only some VIF codes are, and only when stored as a 32-bit constant.",
        "Functions that only fill buffers which the emulated DMA, VIF or GS read later (packet builders) look clean "
        "unless they store a recognised VIF code. Their data still feeds the emulation.",
        "The stub classification is lexical: a handler that reaches emulation through a deeper helper, or names a "
        "subsystem it does not use, is misjudged.",
        "The override scan is lexical too: a registration behind a condition other than an environment variable "
        "counts as on, one whose target is computed at run time is only counted as unresolved, and overlay modules "
        "(replaceModuleFunction) are not in the main ELF's map.",
        "Native C++ means the game's code no longer runs there, not that the replacement is emulation-free: a lifted "
        "VU program can keep VU registers, flags and VU memory, and an override can still call into the runtime's "
        "models.",
        "Runtime-level emulation (the GS front end, the SPU2, the IOP kernel) is not tied to any one function: see the "
        "badges at the top, which come from the port's config.",
        "Coverage only sees what the dispatcher looked up: a function a native override calls directly as C++, or code "
        "of an overlay module sharing an address with another, is attributed to whatever covers that address.",
    ],
}

HTML_TEMPLATE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{{TITLE}}</title>
<style>
:root {
  --bg: #f7f6f2; --surface: #ffffff; --fg: #1f1d1a; --muted: #6a655c; --border: #dedad1; --track: #ebe8e0;
  --accent: #1f6fb2; --focus: #111111;
  --s-native: #1f6fb2; --s-clean: #8cc7b0; --s-direct: #d1495b; --s-indirect: #f0b0a8;
  --s-stub: #a58cd0; --s-stubemu: #5e3c99;
  --b-present: #d1495b; --b-partial: #c98a12; --b-diagnostic: #1f6fb2; --b-absent: #2e8b57;
  color-scheme: light;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg: #15161a; --surface: #1e2025; --fg: #ecebe6; --muted: #a29e95; --border: #34363d; --track: #2a2c32;
    --accent: #5aa9e6; --focus: #ffffff;
    --s-native: #5aa9e6; --s-clean: #2e6b57; --s-direct: #ff6b6b; --s-indirect: #7a3f3f;
    --s-stub: #8a73b8; --s-stubemu: #c7b2f2;
    --b-present: #ff6b6b; --b-partial: #e3a832; --b-diagnostic: #5aa9e6; --b-absent: #4cc38a;
    color-scheme: dark;
  }
}
:root[data-theme="dark"] {
  --bg: #15161a; --surface: #1e2025; --fg: #ecebe6; --muted: #a29e95; --border: #34363d; --track: #2a2c32;
  --accent: #5aa9e6; --focus: #ffffff;
  --s-native: #5aa9e6; --s-clean: #2e6b57; --s-direct: #ff6b6b; --s-indirect: #7a3f3f;
  --s-stub: #8a73b8; --s-stubemu: #c7b2f2;
  --b-present: #ff6b6b; --b-partial: #e3a832; --b-diagnostic: #5aa9e6; --b-absent: #4cc38a;
  color-scheme: dark;
}
* { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; }
body { margin: 0; background: var(--bg); color: var(--fg);
  font: 15px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, Ubuntu, sans-serif; overflow-x: hidden; }
main { max-width: 1180px; margin: 0 auto; padding: 20px 16px 48px; }
h1 { font-size: 22px; line-height: 1.25; margin: 0; }
h2 { font-size: 17px; margin: 32px 0 8px; }
h3 { font-size: 13px; font-weight: 600; margin: 0; }
p { margin: 6px 0; }
code, .mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; font-size: 0.92em; }
.muted { color: var(--muted); }
.top { display: flex; align-items: flex-start; justify-content: space-between; gap: 12px; }
.chip { display: inline-block; border: 1px solid var(--border); border-radius: 999px; padding: 1px 9px;
  font-size: 12px; color: var(--muted); background: var(--surface); }
button { font: inherit; color: inherit; }
.theme { border: 1px solid var(--border); background: var(--surface); border-radius: 8px; padding: 4px 10px;
  cursor: pointer; font-size: 13px; white-space: nowrap; }
.kpi { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1.4fr); gap: 16px; margin-top: 18px; }
.card { background: var(--surface); border: 1px solid var(--border); border-radius: 12px; padding: 14px 16px; min-width: 0; }
.big { font-size: 46px; font-weight: 700; line-height: 1; letter-spacing: -0.02em; color: var(--s-direct); }
.bar { height: 14px; border-radius: 7px; background: var(--track); overflow: hidden; display: flex; margin: 10px 0 6px; }
.bar span { display: block; height: 100%; }
.pct { font-size: 28px; font-weight: 700; }
.badges { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 8px; }
.badge { display: inline-flex; align-items: center; gap: 6px; border: 1px solid var(--border); border-radius: 8px;
  padding: 4px 9px; background: var(--surface); font-size: 13px; cursor: help; max-width: 100%; }
.badge i { width: 9px; height: 9px; border-radius: 50%; flex: none; }
.badge b { font-weight: 600; }
.badge small { color: var(--muted); }
.legend { display: flex; flex-wrap: wrap; gap: 8px; margin: 10px 0; }
.legend button { display: inline-flex; align-items: center; gap: 7px; border: 1px solid var(--border);
  background: var(--surface); border-radius: 8px; padding: 5px 10px; cursor: pointer; font-size: 13px; }
.legend button[aria-pressed="false"] { opacity: 0.45; text-decoration: line-through; }
.legend canvas { width: 14px; height: 14px; flex: none; }
.controls { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
input[type="search"] { font: inherit; color: var(--fg); background: var(--surface); border: 1px solid var(--border);
  border-radius: 8px; padding: 6px 10px; min-width: 0; flex: 1 1 220px; max-width: 360px; }
:focus-visible { outline: 2px solid var(--focus); outline-offset: 2px; }
.details { margin-top: 10px; min-height: 64px; }
.details dl { display: grid; grid-template-columns: max-content minmax(0, 1fr); gap: 2px 12px; margin: 6px 0 0; }
.details dt { color: var(--muted); }
.details dd { margin: 0; overflow-wrap: anywhere; }
.details .links button { border: 1px solid var(--border); background: transparent; border-radius: 6px;
  padding: 0 6px; margin: 2px 4px 2px 0; cursor: pointer; font-size: 12px; }
.region { margin-top: 14px; }
.region .head { display: flex; flex-wrap: wrap; gap: 4px 12px; align-items: baseline; margin-bottom: 4px; }
.region .head .mini { flex: 1 1 120px; max-width: 220px; height: 6px; margin: 0; align-self: center; }
.map { display: block; width: 100%; touch-action: manipulation; cursor: crosshair; }
#tip { position: fixed; z-index: 10; pointer-events: none; max-width: min(420px, calc(100vw - 32px));
  background: var(--surface); color: var(--fg); border: 1px solid var(--border); border-radius: 8px;
  padding: 8px 10px; font-size: 13px; box-shadow: 0 6px 24px rgba(0, 0, 0, 0.18); display: none; }
#tip b { font-weight: 600; }
.irxgrid { display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 10px; }
.chart svg { display: block; width: 100%; height: auto; }
.chart .grid { stroke: var(--border); stroke-width: 1; }
.chart text { fill: var(--muted); font-size: 11px; }
details { margin-top: 10px; }
summary { cursor: pointer; font-weight: 600; }
ul.tight { margin: 6px 0; padding-left: 20px; }
ul.tight li { margin: 4px 0; }
footer { margin-top: 28px; font-size: 12px; color: var(--muted); }
@media (max-width: 720px) {
  .kpi { grid-template-columns: minmax(0, 1fr); }
  .big { font-size: 38px; }
  h1 { font-size: 19px; }
}
</style>
</head>
<body>
<main>
  <div class="top">
    <div>
      <h1 id="heading"></h1>
      <p class="muted" id="sub"></p>
    </div>
    <button class="theme" id="theme" type="button" aria-label="Colour theme">Theme: auto</button>
  </div>

  <section class="kpi" aria-label="Progress">
    <div class="card">
      <h3>Functions still touching emulation</h3>
      <div class="big" id="headline">0</div>
      <p class="muted" id="headline-sub"></p>
    </div>
    <div class="card">
      <h3>Functions emulation-free</h3>
      <div><span class="pct" id="pct"></span> <span class="muted" id="pct-sub"></span></div>
      <div class="bar" id="bar" role="img"></div>
      <p class="muted" id="bar-sub"></p>
    </div>
  </section>

  <h2>Runtime-level emulation</h2>
  <p class="muted" id="runtime-sub"></p>
  <div class="badges" id="runtime"></div>

  <h2>EE function map</h2>
  <p class="muted">One square per function, in address order. Hover for details, click or tap to pin, filter by state
  with the legend.</p>
  <div class="legend" id="legend" role="group" aria-label="Filter by state"></div>
  <div class="controls">
    <input type="search" id="search" placeholder="Search address (0x10f4b8) or name" aria-label="Search address or name">
    <span class="muted" id="found"></span>
  </div>
  <div class="card details" id="details" aria-live="polite"><span class="muted">Nothing pinned yet.</span></div>
  <div id="regions"></div>

  <h2>IRX modules</h2>
  <p class="muted" id="irx-sub"></p>
  <div class="legend" id="irx-legend"></div>
  <div class="irxgrid" id="irx"></div>

  <h2>Trend</h2>
  <p class="muted">Functions still touching emulation (direct and indirect) and direct only, one point per run of
  <code>tools/func_map/func_map.py</code>.</p>
  <div class="card chart" id="chart"></div>

  <h2>How states are decided</h2>
  <div class="card">
    <ul class="tight" id="rules"></ul>
    <details><summary>Known blind spots</summary><ul class="tight" id="blind"></ul></details>
    <details><summary>What the direct touches are</summary><ul class="tight" id="kinds"></ul></details>
  </div>
  <footer id="foot"></footer>
</main>
<div id="tip" role="tooltip"></div>
<script>
const D = /*DATA*/null;
const S = D.strings;
const KEYS = ["native", "clean", "direct", "indirect", "stub"];
const F_UNCERTAIN = 1, F_EMU = 2, F_RAN = 4, F_WRAPPER = 8;
const $ = (id) => document.getElementById(id);
const hex = (n, w = 6) => "0x" + n.toString(16).padStart(w, "0");
const fmt = (n) => n.toLocaleString("en-US");
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
let palette = {};
function readPalette() {
  palette = { bg: css("--surface"), page: css("--bg"), fg: css("--fg"), muted: css("--muted"), border: css("--border") };
  for (const k of KEYS) palette[k] = css("--s-" + k);
  palette.stubemu = css("--s-stubemu");
}
const C = D.counts;
const touching = C.direct + C.indirect;
const enabled = new Set([0, 1, 2, 3, 4]);
let query = null, pinned = null, onlyRan = false;

function stateLabel(f) {
  const st = D.states[f[2]];
  let s = st.label;
  if (f[2] === 4) s += f[5] & F_EMU ? (f[5] & F_UNCERTAIN ? ", possibly emulation-backed" : ", emulation-backed") : ", native";
  else if (f[2] === 2 && f[5] & F_UNCERTAIN) s += " (uncertain)";
  else if (f[2] === 3) s += " (depth " + f[7] + ")";
  else if (f[2] === 0 && f[5] & F_WRAPPER) s += " (wrapper)";
  return s;
}
function viaText(v) {
  const out = [];
  if (v & 1) out.push("hardware access");
  if (v & 2) out.push("VU0 micro mode");
  if (v & 4) out.push("emulation-backed stub");
  return out.join(", ");
}

function header() {
  const m = D.meta;
  $("sub").innerHTML = '<span class="chip">' + esc(m.game) + "</span> " + esc(m.subtitle);
  $("headline").textContent = fmt(touching);
  $("headline-sub").innerHTML = fmt(C.direct) + " touch it themselves (" + fmt(C.uncertain) + " uncertain) and " +
    fmt(C.indirect) + " call one that does (" + fmt(C.indirect_via_vu0_only) + " only through VU0 micro calls), out of " +
    fmt(C.total) + " EE functions. Plus " + fmt(C.stub_emulation) + " library stubs the runtime answers with emulation.";
  const free = C.native + C.clean + C.stub_native;
  $("pct").textContent = m.emulation_free_pct.toFixed(1) + "%";
  $("pct-sub").textContent = fmt(free) + " of " + fmt(C.total);
  const parts = [["native", C.native], ["clean", C.clean], ["stub", C.stub_native], ["stubemu", C.stub_emulation],
    ["indirect", C.indirect], ["direct", C.direct]];
  $("bar").innerHTML = parts.map(([k, n]) => '<span style="width:' + (100 * n / C.total) + "%;background:var(--s-" + k +
    ')" title="' + k + " " + n + '"></span>').join("");
  $("bar").setAttribute("aria-label", m.emulation_free_pct.toFixed(1) + " percent of functions emulation-free");
  $("bar-sub").textContent = "Native C++ " + fmt(C.native) + " (and " + fmt(C.vu_programs_native) +
    " VU program entries), recompiled clean " + fmt(C.clean) + ", native stubs " + fmt(C.stub_native) + ".";
  const status = { present: "present", partial: "partly", diagnostic: "diagnostic only", absent: "absent" };
  $("runtime").innerHTML = D.runtime.map((r) => '<span class="badge" tabindex="0" title="' + esc(r.evidence) +
    '"><i style="background:var(--b-' + r.status + ')"></i><b>' + esc(r.name) + "</b><small>" +
    esc(status[r.status] || r.status) + "</small></span>").join("");
  $("heading").textContent = m.heading;
  $("runtime-sub").textContent = D.runtime.length ? "Not tied to one function: what " + m.short_name +
    "'s defaults still run as a model of the PS2 (hover or tap for the evidence)." :
    "No runtime badges in this port's config.";
  $("foot").textContent = "Generated " + m.generated + " by tools/func_map/func_map.py v" + m.tool_version + " from " +
    m.function_source + ". " + m.short_name + " " + (m.port_commit || "?") + " (" + (m.port_branch || "?") +
    "), PS2Recomp " + (m.ps2recomp_commit || "?") + ". Derived from the game: keep it private.";
}

function drawCell(ctx, x, y, s, f, dim) {
  const st = f[2];
  let col = palette[KEYS[st]];
  if (st === 4 && f[5] & F_EMU) col = palette.stubemu;
  ctx.globalAlpha = dim ? 0.13 : 1;
  if (st === 2 && f[5] & F_UNCERTAIN) {
    ctx.strokeStyle = col; ctx.lineWidth = 1.5;
    ctx.strokeRect(x + 0.75, y + 0.75, s - 1.5, s - 1.5);
  } else {
    ctx.fillStyle = col; ctx.fillRect(x, y, s, s);
    if (st === 2) {
      ctx.strokeStyle = palette.page; ctx.lineWidth = 1.2;
      ctx.beginPath(); ctx.moveTo(x, y + s); ctx.lineTo(x + s, y); ctx.stroke();
    } else if (st === 4 && f[5] & F_EMU) {
      ctx.fillStyle = palette.page; const d = Math.max(1, Math.round(s / 3));
      ctx.fillRect(x + (s - d) / 2, y + (s - d) / 2, d, d);
    }
  }
  ctx.globalAlpha = 1;
}

const maps = [];
function makeMap(container, items, opts) {
  const canvas = document.createElement("canvas");
  canvas.className = "map";
  canvas.setAttribute("role", "img");
  canvas.setAttribute("aria-label", opts.label);
  container.appendChild(canvas);
  const m = { canvas, items, opts, cols: 1, pitch: 7, cell: 6 };
  maps.push(m);
  canvas.addEventListener("mousemove", (e) => hover(m, e));
  canvas.addEventListener("mouseleave", () => { $("tip").style.display = "none"; });
  canvas.addEventListener("click", (e) => { const i = hit(m, e); if (i >= 0) opts.pin(items[i]); });
  return m;
}
function layout(m) {
  const w = m.canvas.parentElement.clientWidth;
  m.pitch = w < 520 ? 6 : 7; m.cell = m.pitch - 1;
  m.cols = Math.max(1, Math.floor((w + 1) / m.pitch));
  const rows = Math.ceil(m.items.length / m.cols);
  const dpr = window.devicePixelRatio || 1;
  const cw = Math.min(w, m.cols * m.pitch), ch = rows * m.pitch;
  m.canvas.style.width = cw + "px"; m.canvas.style.height = ch + "px";
  m.canvas.width = Math.round(cw * dpr); m.canvas.height = Math.round(ch * dpr);
  const ctx = m.canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cw, ch);
  m.items.forEach((f, i) => {
    const x = (i % m.cols) * m.pitch, y = Math.floor(i / m.cols) * m.pitch;
    const dim = m.opts.dim(f);
    m.opts.draw(ctx, x, y, m.cell, f, dim);
    if (m.opts.mark(f)) {
      ctx.strokeStyle = palette.fg; ctx.lineWidth = 1.5;
      ctx.strokeRect(x - 0.25, y - 0.25, m.cell + 0.5, m.cell + 0.5);
    }
  });
}
function hit(m, e) {
  const r = m.canvas.getBoundingClientRect();
  const c = Math.floor((e.clientX - r.left) / m.pitch), row = Math.floor((e.clientY - r.top) / m.pitch);
  if (c < 0 || c >= m.cols) return -1;
  const i = row * m.cols + c;
  return i >= 0 && i < m.items.length ? i : -1;
}
function hover(m, e) {
  const i = hit(m, e), tip = $("tip");
  if (i < 0) { tip.style.display = "none"; return; }
  tip.innerHTML = m.opts.tip(m.items[i]);
  tip.style.display = "block";
  const pad = 14, tw = tip.offsetWidth, th = tip.offsetHeight;
  let x = e.clientX + pad, y = e.clientY + pad;
  if (x + tw > window.innerWidth - 8) x = Math.max(8, e.clientX - tw - pad);
  if (y + th > window.innerHeight - 8) y = Math.max(8, e.clientY - th - pad);
  tip.style.left = x + "px"; tip.style.top = y + "px";
}

function funcTip(f) {
  const name = S[f[3]];
  return "<b class=mono>" + hex(f[0]) + "</b> " + (name ? esc(name) : "") + "<br>" + esc(stateLabel(f)) +
    ", " + fmt(f[1]) + " bytes<br><span class=muted>" + esc(S[f[4]]).slice(0, 360) + "</span>";
}
function pinFunc(f) {
  pinned = f;
  const name = S[f[3]];
  const calls = f[9].map((k) => D.funcs[k]).map((g) => '<button type="button" data-k="' + D.funcs.indexOf(g) + '">' +
    hex(g[0]) + (S[g[3]] ? " " + esc(S[g[3]]) : "") + "</button>").join("");
  $("details").innerHTML = "<h3 class=mono>" + hex(f[0]) + (name ? " " + esc(name) : "") + "</h3><dl>" +
    "<dt>State</dt><dd>" + esc(stateLabel(f)) + "</dd>" +
    "<dt>Range</dt><dd class=mono>" + hex(f[0]) + " - " + hex(f[0] + f[1]) + " (" + fmt(f[1]) + " bytes)</dd>" +
    "<dt>Region</dt><dd>" + esc(S[f[6]]) + "</dd>" +
    "<dt>Why</dt><dd>" + esc(S[f[4]]) + "</dd>" +
    (f[8] ? "<dt>Via</dt><dd>" + viaText(f[8]) + "</dd>" : "") +
    (f[5] & F_RAN ? "<dt>Coverage</dt><dd>ran in the recorded session</dd>" : "") +
    (calls ? "<dt>Calls</dt><dd class=links>" + calls + "</dd>" : "") + "</dl>";
  $("details").querySelectorAll("button[data-k]").forEach((b) => b.addEventListener("click", () => pinFunc(D.funcs[+b.dataset.k])));
  redraw();
}

function legend() {
  const n = [C.native, C.clean, C.direct, C.indirect, C.stub];
  const L = $("legend");
  D.states.forEach((st, i) => {
    const b = document.createElement("button");
    b.type = "button"; b.setAttribute("aria-pressed", "true"); b.title = st.desc;
    const sw = document.createElement("canvas"); sw.width = 28; sw.height = 28;
    b.appendChild(sw);
    b.appendChild(document.createTextNode(st.label + " " + fmt(n[i]) +
      (i === 2 ? " (" + fmt(C.uncertain) + " hollow: uncertain)" : i === 4 ? " (" + fmt(C.stub_emulation) + " dotted: emulation-backed)" : "")));
    b.addEventListener("click", () => {
      if (enabled.has(i)) enabled.delete(i); else enabled.add(i);
      b.setAttribute("aria-pressed", enabled.has(i) ? "true" : "false");
      redraw();
    });
    b._swatch = () => {
      const c = sw.getContext("2d"); c.setTransform(1, 0, 0, 1, 0, 0); c.clearRect(0, 0, 28, 28); c.scale(2, 2);
      drawCell(c, 1, 1, 12, [0, 0, i, 0, 0, i === 4 ? F_EMU : 0], false);
    };
    L.appendChild(b);
  });
}

function regions() {
  const host = $("regions");
  const groups = [];
  D.funcs.forEach((f, k) => {
    const g = groups[groups.length - 1];
    if (g && g.region === f[6]) g.items.push(f); else groups.push({ region: f[6], items: [f] });
  });
  for (const g of groups) {
    const div = document.createElement("div"); div.className = "region";
    const counts = [0, 0, 0, 0, 0]; g.items.forEach((f) => counts[f[2]]++);
    const lo = g.items[0][0], last = g.items[g.items.length - 1], hi = last[0] + last[1];
    const isVu = S[g.region].startsWith("VU ");
    const total = g.items.length;
    div.innerHTML = '<div class="head"><h3>' + esc(S[g.region]) + '</h3><span class="muted mono">' +
      (isVu ? "entries " + hex(lo, 1) + "-" + hex(hi, 1) : hex(lo) + "-" + hex(hi)) + "</span><span class=muted>" +
      fmt(total) + (isVu ? " entries" : " functions") + (counts[2] + counts[3] ? ", " + fmt(counts[2] + counts[3]) + " touching" : "") +
      '</span><div class="bar mini">' + [0, 1, 4, 3, 2].map((s) => '<span style="width:' + (100 * counts[s] / total) +
      "%;background:var(--s-" + KEYS[s] + ')"></span>').join("") + "</div></div>";
    host.appendChild(div);
    makeMap(div, g.items, {
      label: S[g.region] + ": " + total + " squares",
      draw: drawCell,
      dim: (f) => !enabled.has(f[2]) || (query !== null && !query.has(f)) || (onlyRan && !(f[5] & F_RAN)),
      mark: (f) => f === pinned,
      tip: funcTip,
      pin: pinFunc,
    });
  }
}

function coverage() {
  if (!D.meta.coverage) return;
  const b = document.createElement("button");
  b.type = "button"; b.className = "theme"; b.setAttribute("aria-pressed", "false");
  const ran = D.funcs.filter((f) => f[5] & F_RAN).length;
  b.textContent = "Only what ran (" + fmt(ran) + ")";
  b.addEventListener("click", () => { onlyRan = !onlyRan; b.setAttribute("aria-pressed", String(onlyRan)); redraw(); });
  $("search").parentElement.appendChild(b);
  const r = C.ran;
  if (r) {
    const p = document.createElement("p");
    p.className = "muted";
    p.textContent = "Coverage (" + D.meta.coverage_file + "): " + fmt(r.total) + " of " + fmt(C.total) +
      " functions ran, " + fmt(r.direct + r.indirect) + " of them still touching emulation (" + fmt(r.direct) +
      " direct, " + fmt(r.indirect) + " indirect); " + fmt(r.native) + " native, " + fmt(r.clean) + " clean, " +
      fmt(r.stub) + " stubs (" + fmt(r.stub_emulation) + " emulation-backed).";
    $("headline-sub").parentElement.appendChild(p);
  }
}

function search() {
  const box = $("search");
  const run = () => {
    const q = box.value.trim().toLowerCase();
    if (!q) { query = null; $("found").textContent = ""; redraw(); return; }
    const m = q.match(/^(?:0x)?([0-9a-f]{3,8})$/);
    const set = new Set();
    if (m) {
      const a = parseInt(m[1], 16);
      for (const f of D.funcs) if ((a >= f[0] && a < f[0] + f[1]) || f[0].toString(16).includes(m[1])) set.add(f);
    }
    for (const f of D.funcs) if (S[f[3]].toLowerCase().includes(q)) set.add(f);
    query = set;
    $("found").textContent = fmt(set.size) + " match" + (set.size === 1 ? "" : "es");
    redraw();
  };
  box.addEventListener("input", run);
  box.addEventListener("keydown", (e) => {
    if (e.key !== "Enter" || !query || !query.size) return;
    const f = [...query][0];
    pinFunc(f);
    for (const m of maps) {
      const i = m.items.indexOf(f);
      if (i >= 0) { m.canvas.scrollIntoView({ block: "center", behavior: "smooth" }); break; }
    }
  });
}

const IRX_STATES = [["recompiled", "Recompiled", "--s-clean"], ["interpreted", "Interpreted fallback", "--s-direct"],
  ["overridden", "Export natively overridden", "--s-native"]];
function irx() {
  const ic = D.irx_counts;
  const total = ic.recompiled + ic.interpreted + ic.overridden;
  $("irx-sub").textContent = D.irx.length ? fmt(total) + " functions in " + D.irx.length +
    " IRX modules recompiled by tools/iop_recomp.py. They still run on the emulated IOP kernel (see the badge above)." :
    "No generated/iop/ tree was found: run tools/iop_recomp.py over the port's IRX modules, then this tool again.";
  $("irx-legend").innerHTML = IRX_STATES.map(([k, l, v]) => '<span class="badge"><i style="background:var(' + v +
    ');border-radius:2px"></i>' + l + " " + fmt(ic[k] || 0) + "</span>").join("");
  for (const m of D.irx) {
    const card = document.createElement("div"); card.className = "card";
    const n = [0, 0, 0]; m.funcs.forEach((f) => n[f[2]]++);
    card.innerHTML = "<h3>" + esc(m.module) + '</h3><p class="muted mono" style="margin:2px 0 6px">base ' +
      hex(m.base, 5) + ", " + fmt(m.funcs.length) + " functions" + (n[1] ? ", " + n[1] + " interpreted" : "") + "</p>";
    $("irx").appendChild(card);
    const colors = IRX_STATES.map((s) => s[2]);
    makeMap(card, m.funcs, {
      label: m.module + ": " + m.funcs.length + " functions",
      draw: (ctx, x, y, s, f, dim) => {
        ctx.globalAlpha = dim ? 0.13 : 1; ctx.fillStyle = css(colors[f[2]]); ctx.fillRect(x, y, s, s);
        if (f[2] === 1) { ctx.strokeStyle = palette.page; ctx.beginPath(); ctx.moveTo(x, y + s); ctx.lineTo(x + s, y); ctx.stroke(); }
        ctx.globalAlpha = 1;
      },
      dim: () => false,
      mark: (f) => f === pinned,
      tip: (f) => "<b>" + esc(m.module) + "</b> <span class=mono>" + hex(f[0], 5) + "</span>, " + fmt(f[1]) +
        " bytes<br>" + IRX_STATES[f[2]][1] + "<br><span class=muted>" + esc(S[f[3]]) + "</span>",
      pin: (f) => {
        pinned = f;
        $("details").innerHTML = "<h3>" + esc(m.module) + " <span class=mono>" + hex(f[0], 5) + "</span></h3><dl><dt>State</dt><dd>" +
          IRX_STATES[f[2]][1] + "</dd><dt>Size</dt><dd>" + fmt(f[1]) + " bytes</dd><dt>Why</dt><dd>" + esc(S[f[3]]) + "</dd></dl>";
        redraw();
      },
    });
  }
}

function chart() {
  const H = D.history || [];
  const W = 640, Hh = 220, l = 46, r = 14, t = 14, b = 30;
  const pts = H.map((h) => ({ d: h.date.slice(0, 10), all: h.counts.direct + h.counts.indirect, dir: h.counts.direct }));
  const max = Math.max(10, ...pts.map((p) => p.all)) * 1.1;
  const X = (i) => pts.length < 2 ? l + (W - l - r) / 2 : l + i * (W - l - r) / (pts.length - 1);
  const Y = (v) => t + (Hh - t - b) * (1 - v / max);
  let s = '<svg viewBox="0 0 ' + W + " " + Hh + '" role="img" aria-label="Functions touching emulation per run">';
  for (let k = 0; k <= 4; k++) {
    const v = Math.round(max * k / 4);
    s += '<line class="grid" x1="' + l + '" x2="' + (W - r) + '" y1="' + Y(v) + '" y2="' + Y(v) + '"/>' +
      '<text x="' + (l - 6) + '" y="' + (Y(v) + 4) + '" text-anchor="end">' + fmt(v) + "</text>";
  }
  const line = (key, color, dash) => {
    if (pts.length > 1) s += '<polyline fill="none" stroke="var(' + color + ')" stroke-width="2.5"' + (dash ? ' stroke-dasharray="5 4"' : "") +
      ' points="' + pts.map((p, i) => X(i) + "," + Y(p[key])).join(" ") + '"/>';
    pts.forEach((p, i) => { s += '<circle cx="' + X(i) + '" cy="' + Y(p[key]) + '" r="4" fill="var(' + color + ')"><title>' +
      p.d + ": " + fmt(p[key]) + "</title></circle>"; });
  };
  line("all", "--s-direct", false);
  line("dir", "--s-native", true);
  const step = Math.max(1, Math.ceil(pts.length / 6));
  pts.forEach((p, i) => { if (i % step === 0 || i === pts.length - 1) s += '<text x="' + X(i) + '" y="' + (Hh - 10) + '" text-anchor="middle">' + p.d + "</text>"; });
  s += "</svg>";
  const note = pts.length < 2 ? '<p class="muted">One run so far: the line appears with the next one.</p>' : "";
  $("chart").innerHTML = s + '<div class="legend"><span class="badge"><i style="background:var(--s-direct)"></i>direct + indirect</span>' +
    '<span class="badge"><i style="background:var(--s-native)"></i>direct only (dashed)</span></div>' + note;
}

function texts() {
  $("rules").innerHTML = D.heuristics.rules.map(([h, t]) => "<li><b>" + esc(h) + ".</b> " + esc(t) + "</li>").join("");
  $("blind").innerHTML = D.heuristics.blind.map((t) => "<li>" + esc(t) + "</li>").join("");
  $("kinds").innerHTML = Object.entries(D.kinds).map(([k, n]) => "<li>" + esc(k) + ": " + fmt(n) + " functions</li>").join("");
}

function redraw() {
  readPalette();
  maps.forEach(layout);
  document.querySelectorAll("#legend button").forEach((b) => b._swatch && b._swatch());
}

function theme() {
  const btn = $("theme"), root = document.documentElement;
  const order = ["auto", "light", "dark"];
  let mode = "auto";
  try { mode = localStorage.getItem("ps2x-funcmap-theme") || "auto"; } catch (e) { mode = "auto"; }
  const apply = () => {
    if (mode === "auto") root.removeAttribute("data-theme"); else root.setAttribute("data-theme", mode);
    btn.textContent = "Theme: " + mode;
    redraw();
  };
  btn.addEventListener("click", () => {
    mode = order[(order.indexOf(mode) + 1) % order.length];
    try { localStorage.setItem("ps2x-funcmap-theme", mode); } catch (e) { }
    apply();
  });
  if (window.matchMedia) window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => redraw());
  apply();
}

header(); legend(); regions(); irx(); chart(); texts(); search(); coverage();
theme();
let resizeTimer = 0;
window.addEventListener("resize", () => { clearTimeout(resizeTimer); resizeTimer = setTimeout(redraw, 120); });
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
