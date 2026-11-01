"""Tests for benchlock.py: each runs the script against its own state directory. python3 -m unittest test_benchlock,
or benchlock.py --self-test."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "benchlock.py")
EX_TEMPFAIL = 75


class BenchlockTest(unittest.TestCase):
    def setUp(self):
        self.state = tempfile.mkdtemp(prefix="benchlock-test-")
        self.env = dict(os.environ, BENCHLOCK_STATE_DIR=self.state)
        self.env.pop("BENCHLOCK_ANNOUNCE", None)
        self.ids = []
        self.procs = []

    def tearDown(self):
        for lease_id in self.ids:
            self.bl("release", "--id", lease_id)
        for proc in self.procs:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
        shutil.rmtree(self.state, ignore_errors=True)

    def bl(self, *args, timeout=60):
        return subprocess.run([sys.executable, SCRIPT, *args], env=self.env, capture_output=True, text=True,
                              timeout=timeout)

    def spawn(self, *args):
        proc = subprocess.Popen([sys.executable, SCRIPT, *args], env=self.env, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        self.procs.append(proc)
        return proc

    def take(self, tag, mode, *extra, expect="1"):
        result = self.bl("take", "-t", tag, "-m", mode, "-w", "test", "-e", expect, *extra)
        if result.returncode == 0:
            self.ids.append(result.stdout.split()[0])
        return result

    def status(self):
        return self.bl("status").stdout

    def wait_for(self, predicate, seconds=20):
        give_up = time.time() + seconds
        while time.time() < give_up:
            if predicate():
                return True
            time.sleep(0.2)
        return False

    def test_bench_is_exclusive(self):
        self.assertEqual(self.take("A", "bench").returncode, 0)
        self.assertEqual(self.take("B", "bench", "--wait", "0").returncode, EX_TEMPFAIL)
        self.assertEqual(self.take("B", "comp", "--wait", "0").returncode, EX_TEMPFAIL)

    def test_comp_is_shared_but_not_with_bench(self):
        self.assertEqual(self.take("A", "comp").returncode, 0)
        self.assertEqual(self.take("B", "comp", "--wait", "0").returncode, 0)
        self.assertEqual(self.take("C", "bench", "--wait", "0").returncode, EX_TEMPFAIL)
        self.assertEqual(len(re.findall(r"^comp ", self.status(), re.M)), 2)

    def test_remote_host_comp_is_exclusive_and_separate(self):
        self.assertEqual(self.take("A", "comp", "--host", "buildbox").returncode, 0)
        self.assertEqual(self.take("B", "comp", "--host", "buildbox", "--wait", "0").returncode, EX_TEMPFAIL)
        self.assertEqual(self.take("B", "bench", "--wait", "0").returncode, 0)

    def test_comp_slots_limit(self):
        self.assertEqual(self.bl("config", "--comp-slots", "1").returncode, 0)
        self.assertEqual(self.take("A", "comp").returncode, 0)
        self.assertEqual(self.take("B", "comp", "--wait", "0").returncode, EX_TEMPFAIL)
        self.bl("release", "--id", self.ids.pop())
        self.assertEqual(self.take("B", "comp", "--wait", "0").returncode, 0)

    def test_run_is_killed_at_its_deadline(self):
        start = time.time()
        result = self.bl("run", "-t", "A", "-m", "comp", "-w", "test", "-e", "0.01", "-d", "0.05", "--",
                         sys.executable, "-c", "import time; time.sleep(60)")
        self.assertEqual(result.returncode, 124)
        self.assertLess(time.time() - start, 20)
        self.assertEqual(self.status().strip(), "free")
        self.assertIn("deadline reached", open(os.path.join(self.state, "journal.log")).read().lower())

    def test_take_ends_at_its_deadline(self):
        self.assertEqual(self.take("A", "bench", "-d", "0.05", expect="0.01").returncode, 0)
        self.assertTrue(self.wait_for(lambda: self.status().strip() == "free"))
        self.assertEqual(self.take("B", "bench", "--wait", "0").returncode, 0)

    def test_run_returns_its_command_status(self):
        result = self.bl("run", "-t", "A", "-m", "comp", "-w", "test", "-e", "1", "--",
                         sys.executable, "-c", "import sys; sys.exit(7)")
        self.assertEqual(result.returncode, 7)

    def test_release_by_tag_refused_when_tag_holds_several(self):
        self.assertEqual(self.take("A", "comp").returncode, 0)
        self.assertEqual(self.take("A", "comp").returncode, 0)
        result = self.bl("release", "-t", "A")
        self.assertEqual(result.returncode, 2)
        self.assertIn("--id", result.stderr)
        self.assertEqual(len(re.findall(r"^comp ", self.status(), re.M)), 2)
        first = self.ids.pop(0)
        self.assertEqual(self.bl("release", "--id", first).returncode, 0)
        self.assertEqual(self.bl("release", "-t", "A").returncode, 0)
        self.ids.clear()
        self.assertEqual(self.status().strip(), "free")

    def test_waiters_go_in_arrival_order_and_can_yield(self):
        self.assertEqual(self.take("H", "bench").returncode, 0)
        first = self.spawn("run", "-t", "A", "-m", "bench", "-w", "first", "-e", "1", "--",
                           sys.executable, "-c", "pass")
        self.assertTrue(self.wait_for(lambda: "A - first" in self.status()))
        second = self.spawn("run", "-t", "B", "-m", "bench", "-w", "second", "-e", "1", "--",
                            sys.executable, "-c", "pass")
        self.assertTrue(self.wait_for(lambda: "B - second" in self.status()))
        late_comp = self.take("C", "comp", "--wait", "1")
        self.assertEqual(late_comp.returncode, EX_TEMPFAIL)
        status = self.status()
        self.assertLess(status.index("A - first"), status.index("B - second"))

        b_id, a_id = f"B-{second.pid}", f"A-{first.pid}"
        self.assertEqual(self.bl("yield", "--id", a_id, "--to", b_id).returncode, 1)
        self.assertEqual(self.bl("ask", "--id", b_id, "-e", "2", "-w", "short check").returncode, 0)
        self.assertIn("asks to pass", self.status())
        self.assertEqual(self.bl("yield", "--id", a_id, "--to", b_id).returncode, 0)
        status = self.status()
        self.assertLess(status.index("B - second"), status.index("A - first"))

        self.bl("release", "--id", self.ids.pop())
        self.assertEqual(second.wait(30), 0)
        self.assertEqual(first.wait(30), 0)
        took = re.findall(r"^\S+ \S+ (\w): took", open(os.path.join(self.state, "journal.log")).read(), re.M)
        self.assertEqual(took, ["H", "B", "A"])

    def test_wait_returns_once_free(self):
        self.assertEqual(self.bl("wait", "--wait", "0").returncode, 0)
        self.assertEqual(self.take("A", "comp", "-d", "0.05", expect="0.01").returncode, 0)
        self.assertEqual(self.bl("wait", "--wait", "0").returncode, EX_TEMPFAIL)
        self.assertEqual(self.bl("wait", "--wait", "30").returncode, 0)


if __name__ == "__main__":
    unittest.main()
