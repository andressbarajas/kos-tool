#!/bin/sh
# host/bin2c.sh — convert a binary file to a C uint8_t array
# Usage: bin2c.sh <input.bin> <var_name> <output.c>

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
    xxd -i < "$INPUT"
    printf '};\n'
    printf 'const uint32_t %s_size = %s;\n' "$VAR" "$SIZE"
} > "$OUTPUT"
