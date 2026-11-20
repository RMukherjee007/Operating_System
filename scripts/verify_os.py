#!/usr/bin/env python3
"""
Automated end-to-end verification of FulmenOS inside QEMU (executed inside Docker).
Drives the interactive UART shell over a PTY and validates all OS subsystems.
"""

import os

from qemu_test import boot_qemu, read_until


def main() -> int:
    prompt = "root@fulmen:"
    with boot_qemu() as (master_fd, proc):
        read_until(master_fd, prompt)
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
            ("write /tmp/logic.c 'int main() { int n = 0; int a = 0 && ++n || 1; int b = 1 || ++n ? 7 : 9; return a + b + n; }'", None),
            ("cc /tmp/logic.c", "exited with status 8"),
            ("mkdir /home/testdir", None),
            ("cp /etc/os-release /home/testdir/os.txt", None),
            ("mv /home/testdir/os.txt /home/testdir/./os.txt", None),
            ("grep FulmenOS /home/testdir/os.txt", 'NAME="FulmenOS"'),
            ("stat /home/testdir/os.txt", "Mode: 0644"),
            ("mkdir /home/testdir/sub", None),
            ("mv /home/testdir /home/testdir/sub", "mv: failed"),
            ("cd /home/testdir/sub", None),
            ("rm -r /home/testdir", "(err=-16)"),
            ("cp /etc/os-release .", None),
            ("cat os-release", 'NAME="FulmenOS"'),
            ("write os.txt replacement", None),
            ("mv ../os.txt .", None),
            ("cat os.txt", 'NAME="FulmenOS"'),
            ("rm os.txt", None),
            ("stat os.txt", "not found"),
            ("cd /home", None),
            ("wc /home/testdir/sub/os-release", "/home/testdir/sub/os-release"),
            ("rm -r /home/testdir", None),
            ("ps", "shell"),
            ("free", "Phys RAM:"),
            ("df", "ramfs"),
            ("uptime", "ticks @ 100 Hz"),
        ]

        for line, expected in commands:
            os.write(master_fd, (line + "\n").encode("utf-8"))
            output = read_until(master_fd, prompt).partition("\n")[2]
            if expected and expected not in output:
                raise AssertionError(
                    f"Command {line!r} did not produce expected substring {expected!r}"
                )

        # Clean poweroff
        os.write(master_fd, b"poweroff\n")
        rc = proc.wait(timeout=10.0)
        if rc != 0:
            raise AssertionError(f"QEMU exited with non-zero code {rc}")

        print("\n\n=== ALL FULMENOS VERIFICATION CHECKS PASSED ===")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
