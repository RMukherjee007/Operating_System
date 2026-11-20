"""Shared serial console for the FulmenOS QEMU checks."""

from contextlib import contextmanager
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


def read_until(master_fd: int, pattern: str, timeout: float = 15.0) -> str:
    buf: list[str] = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = ANSI_RE.sub("", "".join(buf))
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


@contextmanager
def boot_qemu(*devices):
    master_fd, slave_fd = pty.openpty()
    cmd = [
        "qemu-system-aarch64",
        "-M", "virt,gic-version=2",
        "-cpu", "cortex-a72",
        "-m", "256M",
        "-nic", "none",
        "-kernel", "build/fulmenos-qemu.elf",
        "-display", "none",
        "-device", "ramfb",
        *devices,
        "-serial", "stdio",
    ]
    try:
        tty.setraw(slave_fd, termios.TCSANOW)
        with subprocess.Popen(cmd, stdin=slave_fd, stdout=slave_fd, stderr=subprocess.STDOUT) as proc:
            os.close(slave_fd)
            slave_fd = -1
            try:
                yield master_fd, proc
            finally:
                if proc.poll() is None:
                    proc.kill()
    finally:
        if slave_fd >= 0:
            os.close(slave_fd)
        os.close(master_fd)
