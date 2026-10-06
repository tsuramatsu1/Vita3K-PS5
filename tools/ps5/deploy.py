#!/usr/bin/env python3
"""Copies the built title onto the console over FTP.

    tools/ps5/deploy.py                     send the title
    PS5_HOST=192.168.1.20 tools/ps5/deploy.py

The console's USB drive is not a path on this machine. /mnt/usb0 exists on the PS5, and the
only way to write there is the jailbreak's FTP server - a plain copy has nowhere to copy to.

Each file is sent beside its destination and renamed into place, so a title interrupted
half way through an upload still has the previous whole file rather than a truncated one.
eboot.bin and param.json go last for the same reason: until they are replaced, the title
the shell can launch is the old, working one.
"""
import os
import sys
from ftplib import FTP, error_perm, error_reply

HOST = os.environ.get("PS5_HOST", "192.168.100.57")
PORT = int(os.environ.get("PS5_FTP_PORT", "2121"))
TITLE_ID = os.environ.get("PS5_TITLE_ID", "PPSA99300")
BUILD = os.environ.get("PS5_BUILD_DIR", os.path.expanduser("~/vita-ps5/build-vita3k-ps5"))
SRC = os.path.join(BUILD, "title", TITLE_ID)
DST = os.environ.get("PS5_TITLE_DIR", "/mnt/usb0/" + TITLE_ID)

# Replaced last, so an interrupted upload leaves a launchable title behind
LAST = ("eboot.bin", "sce_sys/param.json")


def main():
    if not os.path.isdir(SRC):
        sys.exit(f"{SRC} does not exist - build first with tools/ps5/build.sh")

    ftp = FTP()
    try:
        ftp.connect(HOST, PORT, timeout=60)
    except OSError as error:
        sys.exit(f"cannot reach the console at {HOST}:{PORT} ({error})\n"
                 "Is it awake, on the network, and is its FTP server running?")
    ftp.login()

    def make_directory(path):
        try:
            ftp.mkd(path)
        except error_perm:
            pass  # already there

    files = []
    for root, _, names in os.walk(SRC):
        relative = os.path.relpath(root, SRC)
        if relative != ".":
            make_directory(DST + "/" + relative.replace(os.sep, "/"))
        for name in names:
            files.append(os.path.normpath(os.path.join(relative, name)).replace(os.sep, "/"))
    files.sort(key=lambda path: path in LAST)

    for path in files:
        remote = DST + "/" + path
        staged = remote.rsplit("/", 1)[0] + "/.upload." + path.rsplit("/", 1)[-1]
        with open(os.path.join(SRC, path), "rb") as handle:
            ftp.storbinary("STOR " + staged, handle, blocksize=1 << 20)
        try:
            ftp.delete(remote)
        except (error_perm, error_reply):
            # The console's server answers DELE with 226, which ftplib reports as an
            # unexpected reply even though the file is gone. A missing file is fine too
            pass
        ftp.rename(staged, remote)

    print(f"sent {len(files)} files to {HOST}:{DST}")
    # Note: this server's SIZE over-reports (a 56,495,357-byte file comes back as
    # 58,221,432), so it cannot be used to verify an upload. Compare a re-download instead
    ftp.quit()


if __name__ == "__main__":
    main()
