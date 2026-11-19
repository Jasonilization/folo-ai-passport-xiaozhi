#!/usr/bin/env python3
"""Pack a WAD into a random-access lump container for the ESP32-C3 port.

Output format (all little-endian):
  magic "WADP" | u32 numlumps | u32 blob_offset
  dir[numlumps]: char name[8] | u32 blob_off | u32 comp_size | u32 orig_size
  blob: raw-deflate stream per lump (zlib wbits=-15), concatenated

Random access = tinfl on one lump's stream. No network, no big RAM.
"""
import struct, sys, zlib

def main(src, dst):
    d = open(src, 'rb').read()
    magic, num, dirofs = struct.unpack('<4sII', d[:12])
    assert magic == b'IWAD', magic
    lumps = []
    for i in range(num):
        off, size, name = struct.unpack('<II8s', d[dirofs + i * 16: dirofs + i * 16 + 16])
        lumps.append((off, size, name.rstrip(b'\0').decode('latin1')))
    header_size = 12 + num * 24
    blob = bytearray()
    dir_entries = []
    for off, size, name in lumps:
        raw = d[off:off + size]
        comp = zlib.compressobj(9, zlib.DEFLATED, -15)
        c = comp.compress(raw) + comp.flush()
        dir_entries.append((name, len(blob), len(c), size))
        blob += c
    out = bytearray()
    out += struct.pack('<4sII', b'WADP', num, header_size)
    for name, boff, csize, osize in dir_entries:
        out += name.encode('latin1').ljust(8, b'\0') + struct.pack('<III', boff, csize, osize)
    out += blob
    open(dst, 'wb').write(out)
    print(f'{src}: {num} lumps, raw={len(d)}, packed={len(out)} ({len(out)/len(d)*100:.0f}%)')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
