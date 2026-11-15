#!/usr/bin/env python3
"""Re-sign a Harry Potter and the Philosopher's Stone save for another key: the port's saves for a real Xbox.

A PS save file ("Save Game N" in UDATA\\45410037\\<save dir>\\) is a 4-byte size, a 20-byte signature, then the
data. The signature is HMAC-SHA1(XboxSignatureKey, data): the game passes the kernel's XboxSignatureKey straight to
XCalculateSignatureBeginEx's roamable path (sub_00158614 -> sub_001585D3 -> sub_00158477, flags 0), and the kernel
sets that key per title as it starts it. The port's kernel leaves it all zeros, so its saves are signed with 16 zero
bytes; a console's key for this title (the US and PAL editions share the certificate's signature key, so one key
serves both) comes from the running console (xbdm, kernel export 325).

    resign_save.py --check <save file>...              which key signed it: zeros (the port) or not
    resign_save.py <key hex> <save dir> <out dir>      a copy of the save dir, its save file signed with <key>
"""
import hashlib
import hmac
import os
import shutil
import sys

ZERO_KEY = bytes(16)


def signature(key: bytes, blob: bytes) -> bytes:
    return hmac.new(key, blob[24:], hashlib.sha1).digest()


def save_files(folder: str):
    return [os.path.join(folder, f) for f in os.listdir(folder) if f.startswith("Save Game")]


def main(argv):
    if len(argv) >= 2 and argv[0] == "--check":
        for path in argv[1:]:
            blob = open(path, "rb").read()
            zero = signature(ZERO_KEY, blob) == blob[4:24]
            print(f"{path}: {'the port (zero key)' if zero else 'another key (a console?)'}")
        return 0
    if len(argv) != 3:
        print(__doc__)
        return 2
    key = bytes.fromhex(argv[0])
    if len(key) != 16:
        sys.exit("the key is 16 bytes (32 hex digits)")
    src, out = argv[1], argv[2]
    if os.path.exists(out):
        sys.exit(f"{out} exists; give a new folder (nothing is overwritten)")
    shutil.copytree(src, out)
    for path in save_files(out):
        blob = bytearray(open(path, "rb").read())
        blob[4:24] = signature(key, bytes(blob))
        open(path, "wb").write(blob)
        print(f"signed {path}: {blob[4:24].hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
