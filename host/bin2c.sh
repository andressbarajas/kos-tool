#!/bin/sh
# host/bin2c.sh — convert a binary file to a C uint8_t array
# Usage: bin2c.sh <input.bin> <var_name> <output.c>
#
# Uses od + awk (both POSIX) rather than `xxd -i`: xxd ships with macOS but
# is not part of a minimal Linux install (it lives in vim-common), so the
# xxd dependency broke `make host` firmware embedding on stock Linux/CI boxes.

set -e

INPUT="$1"
VAR="$2"
OUTPUT="$3"

if [ ! -f "$INPUT" ]; then
    echo "Error: $INPUT not found" >&2
    exit 1
fi

SIZE=$(wc -c < "$INPUT" | tr -d ' ')

{
    printf '#include <stdint.h>\n'
    printf 'const uint8_t %s_data[] = {\n' "$VAR"
    od -An -v -tx1 "$INPUT" | awk '
        {
            for (i = 1; i <= NF; i++) {
                printf "0x%s,", $i
                if (++n % 12 == 0) printf "\n"
            }
        }
        END { if (n % 12 != 0) printf "\n" }'
    printf '};\n'
    printf 'const uint32_t %s_size = %s;\n' "$VAR" "$SIZE"
} > "$OUTPUT"
