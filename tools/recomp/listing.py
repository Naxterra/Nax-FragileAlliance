"""Prints the discovered instructions of a program in address order, for reading the original code.

Usage: python tools/recomp/listing.py <program> <start in hex> [<end in hex>]

Without an end address the listing covers the chunk that starts at or contains the start address. Each line
shows the address, the instruction and, for addresses inside the data object, the text found there.
"""

import bisect
import sys

import discover32
import emit32
import le


def text_at(image: le.LeImage, address: int) -> str:
    obj = image.object_at(address)
    if obj is None or obj.executable:
        return ""
    raw = bytes(obj.data[address - obj.base:address - obj.base + 64]).split(b"\0")[0]
    if len(raw) >= 3 and all(32 <= ch < 127 for ch in raw):
        return raw.decode("ascii")
    return ""


def main(argv) -> int:
    with open(argv[1], "rb") as handle:
        image = le.load(handle.read())
    found = discover32.discover(image)
    emitter = emit32.Emitter(image, found)
    start = int(argv[2], 16)
    if len(argv) > 3:
        end = int(argv[3], 16)
    else:
        index = bisect.bisect_right(emitter.starts, start) - 1
        chunk = emitter.chunks[index]
        start, end = chunk[0], chunk[-1] + 1
    for address in sorted(a for a in found.instructions if start <= a < end):
        ins = found.instructions[address]
        mark = ">" if address in emitter.start_set else " "
        note = ""
        for part in ins.op_str.replace("[", " ").replace("]", " ").replace(",", " ").split():
            if part.startswith("0x"):
                text = text_at(image, int(part, 16))
                if text:
                    note = f'  ; "{text}"'
        print(f"{mark}{address:08x}  {ins.mnemonic} {ins.op_str}{note}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__.strip())
        sys.exit(2)
    sys.exit(main(sys.argv))
