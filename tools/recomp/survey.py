"""Prints what code discovery finds in a bound LE program and what it asks of DOS and the hardware.

Usage: python tools/recomp/survey.py <path to FRAGILE.EXE>
"""

import sys

import discover32
import le


def main(path: str) -> int:
    with open(path, "rb") as handle:
        image = le.load(handle.read())
    print(f"application stub at {image.app_offset:#x}, LE header at {image.header_offset:#x}")
    print(f"entry {image.entry_linear:#x}, stack {image.stack_linear:#x}, fixups {len(image.fixups)}")
    for obj in image.objects:
        kind = "code" if obj.executable else "data"
        bits = 32 if obj.big else 16
        print(f"  object {obj.index}: {kind}, {bits}-bit, {obj.base:#x}..{obj.end:#x}")
    found = discover32.discover(image)
    for obj in image.objects:
        if obj.executable and obj.big:
            covered = found.covered_bytes(obj)
            print(f"object {obj.index}: {covered} of {obj.virtual_size} bytes reached "
                  f"({100 * covered / obj.virtual_size:.1f} %)")
    print(f"instructions {len(found.instructions)}, functions {len(found.functions)}, "
          f"jump tables {len(found.jump_tables)}")
    print(f"conflicts {len(found.conflicts)}, undecodable {len(found.undecodable)}, "
          f"targets outside code {len(found.outside)}, unresolved indirect branches "
          f"{len(found.unresolved_indirect)}")
    print("software interrupts (number, function: count):")
    for (number, function), count in sorted(found.interrupts.items(), key=lambda x: (x[0][0], x[0][1] or -1)):
        shown = f"{function:04x}" if function is not None else "unknown"
        print(f"  {number:02x} {shown}: {count}")
    print("port access (instruction, port: count):")
    for (mnemonic, port), count in sorted(found.ports.items(), key=lambda x: (x[0][0], x[0][1] or -1)):
        shown = f"{port:#05x}" if port is not None else "from dx"
        print(f"  {mnemonic} {shown}: {count}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__.strip())
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
