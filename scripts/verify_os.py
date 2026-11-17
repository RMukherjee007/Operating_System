#!/usr/bin/env python3
"""
Automated end-to-end verification of FulmenOS inside QEMU (executed inside Docker).
Drives the interactive UART shell over a PTY and validates all OS subsystems.
"""

import os
import pty
import re
import select
import subprocess
import sys
import termios
import time
import tty

ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def strip_ansi(s: str) -> str:
    return ANSI_RE.sub("", s)


def read_until(master_fd: int, pattern: str, buf: list[str], timeout: float = 10.0) -> str:
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = strip_ansi("".join(buf))
        if pattern in text:
            return text
        rlist, _, _ = select.select([master_fd], [], [], 0.1)
        if master_fd in rlist:
            try:
                chunk = os.read(master_fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            decoded = chunk.decode("utf-8", errors="replace")
            sys.stdout.write(decoded)
            sys.stdout.flush()
            buf.append(decoded)
    raise TimeoutError(f"Timed out waiting for pattern: {pattern!r}")


def main() -> int:
    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd, termios.TCSANOW)
    cmd = [
        "qemu-system-aarch64",
        "-M", "virt,gic-version=2",
        "-cpu", "cortex-a72",
        "-m", "256M",
        "-nic", "none",
        "-kernel", "build/fulmenos-qemu.elf",
        "-display", "none",
        "-device", "ramfb",
        "-serial", "stdio",
    ]
    proc = subprocess.Popen(
        cmd,
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=subprocess.STDOUT,
        close_fds=True,
    )
    os.close(slave_fd)

    buf: list[str] = []
    prompt = "root@fulmen:"

    try:
        read_until(master_fd, prompt, buf, timeout=15.0)
        buf.clear()

        commands = [
            ("uname -a", "FulmenOS fulmen 1.0.0"),
            ("ls -l /", "bin/"),
            ("ls -l /bin", "hello"),
            ("ls -l /src", "calc.cpp"),
            ("mem", "ENABLED"),
            ("pagetable 0x40200000", "EL1 RW"),
            ("pagetable 0x44000000", "EL0+EL1 RW"),
            ("pagetable 0x09000000", "Device-nGnRnE"),
            ("alloc_test", "[PASS]"),
            ("sched rr", "RR"),
            ("sched_test", "All 3 worker threads completed under RR"),
            ("sched prio", "PRIO"),
            ("sched_test", "All 3 worker threads completed under PRIO"),
            ("sched mlfq", "MLFQ"),
            ("sched_test", "All 3 worker threads completed under MLFQ"),
            ("mutex_test", "[PASS]"),
            ("sem_test", "[PASS]"),
            ("deadlock_test", "[DEADLOCK PREVENTED]"),
            ("el0_test", "Hardware Exception Level=EL0"),
            ("hello arg1 arg2", "[EL0 User Program]"),
            ("fib 12", "0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89"),
            ("primes 50", "Total primes found: 15"),
            ("calc", "Calculator::factorial(10) = 3628800"),
            ("cc /src/demo.c", "fact(7) = 5040"),
            (
                'write /home/live.c "int main() { for (int i = 1; i <= 5; i = i + 1) { printf(\\"Cube of %d is %d\\\\n\\", i, i*i*i); } return 0; }"',
                None,
            ),
            ("cc /home/live.c", "Cube of 5 is 125"),
            ("mkdir /home/testdir", None),
            ("cp /etc/os-release /home/testdir/os.txt", None),
            ("grep FulmenOS /home/testdir/os.txt", "FulmenOS"),
            ("wc /home/testdir/os.txt", "/home/testdir/os.txt"),
            ("rm -r /home/testdir", None),
            ("ps", "shell"),
            ("free", "Phys RAM:"),
            ("df", "ramfs"),
            ("uptime", "ticks @ 100 Hz"),
        ]

        for line, expected in commands:
            buf.clear()
            os.write(master_fd, (line + "\n").encode("utf-8"))
            output = read_until(master_fd, prompt, buf, timeout=15.0)
            if expected and expected not in output:
                raise AssertionError(
                    f"Command {line!r} did not produce expected substring {expected!r}"
                )

        # Clean poweroff
        buf.clear()
        os.write(master_fd, b"poweroff\n")
        rc = proc.wait(timeout=10.0)
        if rc != 0:
            raise AssertionError(f"QEMU exited with non-zero code {rc}")

        print("\n\n=== ALL FULMENOS VERIFICATION CHECKS PASSED ===")
        return 0

    except Exception as exc:
        print(f"\n[VERIFY ERROR] {exc}", file=sys.stderr)
        proc.kill()
        return 1
    finally:
        try:
            os.close(master_fd)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
