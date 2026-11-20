#!/usr/bin/env python3
"""Exercise desktop keyboard navigation, the GUI terminal, and shutdown."""

import os

from qemu_test import boot_qemu, read_until


def main() -> int:
    with boot_qemu("-device", "virtio-keyboard-device", "-device", "virtio-tablet-device") as (fd, proc):
        read_until(fd, "Launching FulmenOS Classic Macintosh Desktop")
        # Tab twice to select Terminal, then Enter to open it.
        os.write(fd, b"\t\t\runame -a\r")
        read_until(fd, "FulmenOS fulmen 1.0.0")
        os.write(fd, b"calc\r")
        read_until(fd, "Calculator::factorial(10) = 3628800")
        os.write(fd, b"\x17\x11")  # Close the window and shut down.
        rc = proc.wait(timeout=10.0)
        if rc != 0:
            raise AssertionError(f"QEMU exited with non-zero code {rc}")
    print("\n=== GUI VERIFICATION PASSED ===")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
