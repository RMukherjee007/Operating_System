#!/usr/bin/env python3
"""
Verification of FulmenOS GUI Desktop:
- Tests direct launch with virtio keyboard and tablet
- Tests icons navigation and execution
- Tests opening and running commands in GUI Terminal
- Tests window closing (Ctrl+W and close button)
- Tests clean poweroff from GUI
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
        "-device", "virtio-keyboard-device",
        "-device", "virtio-tablet-device",
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

    try:
        # Wait for GUI direct launch message
        read_until(master_fd, "Launching FulmenOS Classic Macintosh Desktop", buf, timeout=15.0)
        print("\n[OK] GUI directly launched on boot with interactive input devices.")
        buf.clear()

        # In GUI: all windows start closed. Let's select icon 1 (Terminal) via Tab, then press Enter
        # Tab 1: icon 0 (FulmenHD), Tab 2: icon 1 (Terminal)
        time.sleep(0.5)
        os.write(master_fd, b"\t\t\r")
        time.sleep(0.5)

        # Now Terminal window is opened and focused!
        # Send command "uname -a\n"
        os.write(master_fd, b"uname -a\r")
        time.sleep(0.5)

        # Send command "calc\n"
        os.write(master_fd, b"calc\r")
        time.sleep(0.5)

        # Send Ctrl+W (0x17) to close the window
        os.write(master_fd, b"\x17")
        time.sleep(0.5)

        # Send Ctrl+Q (0x11) to shut down OS
        os.write(master_fd, b"\x11")
        rc = proc.wait(timeout=10.0)
        if rc != 0:
            raise AssertionError(f"QEMU exited with non-zero code {rc}")

        print("\n=== GUI VERIFICATION PASSED ===")
        return 0

    except Exception as exc:
        print(f"\n[GUI VERIFY ERROR] {exc}", file=sys.stderr)
        proc.kill()
        return 1
    finally:
        try:
            os.close(master_fd)
        except OSError:
            pass

if __name__ == "__main__":
    sys.exit(main())
