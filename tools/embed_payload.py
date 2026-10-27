#!/usr/bin/env python3
"""Embed a locally built ELF in a C object (no network access)."""
import argparse
import hashlib
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('elf', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
data = args.elf.read_bytes()
if len(data) < 64 or data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1 or data[18:20] != b'\x3e\x00':
    raise SystemExit('Not an x86-64 little-endian ELF')
args.output.parent.mkdir(parents=True, exist_ok=True)
with args.output.open('w', encoding='ascii') as out:
    out.write('/* Auto-generated from browser-capture.elf. GPL-3.0-or-later. */\n')
    out.write('#include <stddef.h>\n')
    out.write('const unsigned char ps5_capture_elf[] = {\n')
    for offset in range(0, len(data), 24):
        out.write('  ' + ', '.join(f'0x{b:02x}' for b in data[offset:offset+24]) + ',\n')
    out.write('};\nconst size_t ps5_capture_elf_len = sizeof ps5_capture_elf;\n')
print('Embedded bytes:', len(data), 'SHA256:', hashlib.sha256(data).hexdigest())
