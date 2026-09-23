"""Exercise the actual demo mains with test-only kernel/allocation wrappers."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


ATTACKER = str(Path(sys.argv[1]).resolve())
VICTIM = str(Path(sys.argv[2]).resolve())


def environment(directory, role, failure=None):
    env = os.environ.copy()
    env.update(L2_TEST_DIR=directory, L2_TEST_ROLE=role, L2_TEST_EINTR="1")
    env.pop("L2_TEST_FAIL", None)
    if failure:
        env["L2_TEST_FAIL"] = failure
    return env


def events(directory, role):
    path = Path(directory, role + ".trace")
    return path.read_text().splitlines() if path.exists() else []


def paired(selected, attacker_failure=None, victim_failure=None):
    samples = 4
    with tempfile.TemporaryDirectory(prefix="l2-demos-") as directory:
        attacker = subprocess.Popen(
            [ATTACKER, str(samples)], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True,
            env=environment(directory, "attacker", attacker_failure))
        victim = None
        try:
            deadline = time.monotonic() + 10
            while "wait-ready" not in events(directory, "attacker"):
                assert attacker.poll() is None, attacker.communicate()
                assert time.monotonic() < deadline, "attacker never became ready"
                time.sleep(0.01)
            victim = subprocess.Popen(
                [VICTIM, str(selected)], stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True,
                env=environment(directory, "victim", victim_failure))
            out, err = attacker.communicate(timeout=10)
            victim_out, victim_err = victim.communicate(timeout=10)
            assert attacker.returncode == (1 if attacker_failure else 0), err
            assert victim.returncode == (1 if victim_failure else 0), victim_err
            a = events(directory, "attacker")
            v = events(directory, "victim")
            assert a.count("pin 0") == 1 and a.count("warmup") == 1, a
            assert a.count("destroy") == 1 and v.count("destroy") == 1
            assert "create 1 1" in v and f"set {selected}" in v, v
            assert v.index("isolation 1") < v.index("create 1 1"), v
            assert v.index("destroy") < v.index("isolation 0"), v
            assert v.count("ready") == 1 and v.count("stop") == 1, v
            commands = [line for line in v if line.startswith("command ")]
            if attacker_failure:
                assert commands == [] and "Results over" not in out
            else:
                assert commands == ["command 0", "command 1"] * samples, v
                assert f"Results over {samples} samples:" in out, out
                assert re.findall(r"^Set (\d+)$", out, re.M) == ["30", "60", "99"]
                assert out.count("Baseline:") == out.count("With touch:") == 3
                assert out.count("Difference:") == 3
                assert re.search(r"victim touched set (30|60|99)\n", out), out
            assert v.count("done") == len(commands) + 1, v
            assert "Disabling L2 isolation..." in victim_out
            assert not Path(directory, "ipc").exists(), "IPC allocation leaked"
        finally:
            for process in (attacker, victim):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.communicate()


def invalid_arguments():
    cases = [(ATTACKER, "attacker", args) for args in
             (["0"], ["-1"], ["bad"], ["12junk"], ["4294967296"], ["1", "2"])]
    cases += [(VICTIM, "victim", args) for args in
              ([], ["31"], ["30junk"], ["4294967326"], ["30", "60"])]
    for binary, role, args in cases:
        with tempfile.TemporaryDirectory(prefix="l2-invalid-") as directory:
            result = subprocess.run([binary, *args], capture_output=True,
                                    text=True, timeout=5,
                                    env=environment(directory, role))
            assert result.returncode == 1, (args, result)
            assert events(directory, role) == [], (args, "setup before validation")


def setup_failures():
    for failure in ("isolation-enable", "create", None):
        with tempfile.TemporaryDirectory(prefix="l2-failure-") as directory:
            result = subprocess.run([VICTIM, "30"], capture_output=True,
                                    text=True, timeout=5,
                                    env=environment(directory, "victim", failure))
            assert result.returncode == 1, result
            log = events(directory, "victim")
            if failure == "isolation-enable":
                assert log == ["isolation 1"], log
            else:
                assert log[0] == "isolation 1" and log[-1] == "isolation 0", log
                assert log.count("destroy") == (0 if failure == "create" else 1)
    with tempfile.TemporaryDirectory(prefix="l2-failure-") as directory:
        result = subprocess.run([ATTACKER, "4"], capture_output=True,
                                text=True, timeout=5,
                                env=environment(directory, "attacker", "create"))
        assert result.returncode == 1 and not Path(directory, "ipc").exists()


invalid_arguments()
setup_failures()
for target in (30, 60, 99):
    paired(target)
paired(30, attacker_failure="warmup")
paired(60, victim_failure="isolation-disable")
print("PASS: demo arguments, paired rounds, EINTR, shutdown, and isolation cleanup")
print("Allocation, affinity, warm-up, and prctl were simulated; IPC and probes ran.")
