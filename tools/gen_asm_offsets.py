#!/usr/bin/env python3
"""Turn the assembler output of kernel/arch/offsets.c into a NASM include.

kernel/arch/offsets.c is compiled with -S, never assembled. Each ASM_DEFINE()
leaves a line like

    .ascii "@ASMDEF@ CPU_LOCAL_KERNEL_RSP_TOP_OFF 16"

in the .s file. This script lifts those pairs out, in source order, and writes
them as %define directives.

Usage: gen_asm_offsets.py <input.s> <output.inc>
"""

import re
import sys

MARKER = re.compile(r'@ASMDEF@\s+([A-Za-z_][A-Za-z0-9_]*)\s+(-?\d+)')

HEADER = """; build/generated/asm_offsets.inc , GENERATED FILE, DO NOT EDIT.
;
; Produced from kernel/arch/offsets.c by tools/gen_asm_offsets.py. Every value
; here comes from offsetof()/sizeof() on the real kernel structures, so a
; struct change is reflected in the assembly on the next build instead of
; drifting silently. Edit kernel/arch/offsets.c, not this file.

%ifndef ASM_OFFSETS_INC
%define ASM_OFFSETS_INC

"""

FOOTER = """
%endif ; ASM_OFFSETS_INC
"""


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gen_asm_offsets.py <input.s> <output.inc>\n")
        return 2

    src, dst = argv[1], argv[2]

    with open(src, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()

    seen = {}
    names = []
    for name, value in MARKER.findall(text):
        value = int(value)
        if name in seen:
            if seen[name] != value:
                sys.stderr.write(
                    "gen_asm_offsets: %s defined twice with %d and %d\n"
                    % (name, seen[name], value))
                return 1
            continue
        seen[name] = value
        names.append(name)

    if not names:
        sys.stderr.write(
            "gen_asm_offsets: no @ASMDEF@ markers in %s , did the compiler "
            "optimise them away, or did the ASM_DEFINE macro change?\n" % src)
        return 1

    width = max(len(n) for n in names)
    lines = ["%%define %-*s %d" % (width, n, seen[n]) for n in names]

    with open(dst, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(HEADER)
        handle.write("\n".join(lines))
        handle.write(FOOTER)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
