#!/usr/bin/env python3
"""Fetches the emulator's log off the console.

    tools/ps5/logs.py              this run's log, summarised
    tools/ps5/logs.py --previous   the run before it
    tools/ps5/logs.py --full       every line, not a summary

The log on the console is the authoritative record. The kernel log (klog) loses the tail of
a crashing process, because its stderr is a pipe that dies with the process - so a crash
often looks like silence there while the real error sits in this file.

Returning to the game list restarts the title, which would otherwise truncate the log of the
game you just ran. One run back is kept as vita3k.log.previous for exactly that reason, and
is usually the file you want after a game has exited.
"""
import collections
import io
import os
import sys
from ftplib import FTP

HOST = os.environ.get("PS5_HOST", "192.168.100.57")
PORT = int(os.environ.get("PS5_FTP_PORT", "2121"))
LOG_DIR = os.environ.get("PS5_LOG_DIR", "/data/Vita3K/logs")

INTERESTING = (
    "Vulkan device", "Driver version", "memory mapping method", "texture viewport",
    "interlock", "async compilation", "Mapped game memory", "Resolution multiplier",
    "Audio backend", "Presenting to PS5 VideoOut", "Argument",
)


def main():
    name = "vita3k.log.previous" if "--previous" in sys.argv else "vita3k.log"
    ftp = FTP()
    try:
        ftp.connect(HOST, PORT, timeout=60)
    except OSError as error:
        sys.exit(f"cannot reach the console at {HOST}:{PORT} ({error})")
    ftp.login()

    buffer = io.BytesIO()
    try:
        ftp.retrbinary(f"RETR {LOG_DIR}/{name}", buffer.write)
    except Exception as error:
        sys.exit(f"could not read {LOG_DIR}/{name}: {error}")
    ftp.quit()

    lines = buffer.getvalue().decode(errors="replace").splitlines()
    print(f"{name}: {len(lines)} lines")

    if "--full" in sys.argv:
        print("\n".join(lines))
        return

    print("\n-- how this run was configured --")
    for line in lines:
        if any(key in line for key in INTERESTING):
            print("  " + line[:170])

    print("\n-- warnings and errors, by frequency --")
    counts = collections.Counter()
    for line in lines:
        if "|W|" in line or "|E|" in line or "|C|" in line:
            counts[line.split("]: ", 1)[1][:100] if "]: " in line else line[:100]] += 1
    for message, n in counts.most_common(15):
        print(f"  {n:>5}  {message}")

    print("\n-- last 15 lines --")
    for line in lines[-15:]:
        print("  " + line[:200])


if __name__ == "__main__":
    main()
