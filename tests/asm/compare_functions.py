#!/usr/bin/env python3
"""Fails unless the named functions in an object file disassemble to identical code.

usage: compare_functions.py OBJDUMP OBJECT BASELINE_FUNCTION OTHER_FUNCTION...
"""

import re
import subprocess
import sys

HEADER = re.compile(r"^[0-9a-f]+ <(?P<name>[^>]+)>:$")
INSTRUCTION = re.compile(r"^\s+[0-9a-f]+:\s+(?P<text>.+)$")
PADDING = re.compile(r"^(nop|xchg\s+%ax,%ax|data16|cs nopw|int3)")


def function_bodies(objdump: str, obj: str) -> dict[str, list[str]]:
    listing = subprocess.run(
        [objdump, "-d", "--no-show-raw-insn", obj], check=True, capture_output=True, text=True
    ).stdout
    bodies: dict[str, list[str]] = {}
    current: list[str] | None = None
    for line in listing.splitlines():
        if header := HEADER.match(line):
            current = bodies.setdefault(header["name"], [])
        elif current is not None and (instruction := INSTRUCTION.match(line)):
            text = " ".join(instruction["text"].split())
            if not PADDING.match(text):
                current.append(text)
    return bodies


def main() -> int:
    objdump, obj, baseline, *others = sys.argv[1:]
    bodies = function_bodies(objdump, obj)
    print(f"{baseline}: {bodies[baseline]}")
    identical = True
    for name in others:
        print(f"{name}: {bodies[name]}")
        if bodies[name] != bodies[baseline]:
            print(f"MISMATCH: {name} differs from {baseline}")
            identical = False
    return 0 if identical else 1


if __name__ == "__main__":
    sys.exit(main())
