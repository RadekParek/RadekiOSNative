#!/usr/bin/env python3
"""Reproducible host fixtures; Python's plistlib/zlib are used only at generation time."""
import plistlib
import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / 'tests' / 'fixtures.h'

def png_chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))

def fixture(name, data):
    return 'inline const std::vector<uint8_t> ' + name + ' = {' + ','.join(map(str, data)) + '};\n'

data = b'ABCD' * 1024
f = zlib.compressobj(level=6, wbits=-15, strategy=zlib.Z_FIXED)
fixed = f.compress(data) + f.flush()
f = zlib.compressobj(level=6, wbits=-15)
dynamic = f.compress(data + bytes(range(256)) * 6) + f.flush()
info = {'CFBundleDisplayName': 'Example & Friends', 'CFBundleIdentifier': 'org.example.demo',
        'CFBundleExecutable': 'Demo', 'CFBundleIconFiles': ['DemoIcon'], 'Items': ['abc', 'def'],
        'Version': 17, 'Flag': True, 'Unicode': '🚀'}
raw = bytes([0, 6, 12, 25, 128])  # BGRA premultiplied for RGBA (50,24,12,128)
cgbi = (b'\x89PNG\r\n\x1a\n' + png_chunk(b'CgBI', b'\x50\x00\x20\x02') +
        png_chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0)) +
        png_chunk(b'IDAT', zlib.compress(raw, wbits=-15)) + png_chunk(b'IEND', b''))
content = '#pragma once\n#include <cstdint>\n#include <vector>\nnamespace radeki::fixtures {\n'
for name, value in [('fixed', fixed), ('dynamic', dynamic), ('fixedExpected', data),
                    ('dynamicExpected', data + bytes(range(256)) * 6),
                    ('binaryPlist', plistlib.dumps(info, fmt=plistlib.FMT_BINARY)),
                    ('xmlPlist', plistlib.dumps(info, fmt=plistlib.FMT_XML)), ('cgbi', cgbi)]:
    content += fixture(name, value)
OUT.write_text(content + '}\n')
