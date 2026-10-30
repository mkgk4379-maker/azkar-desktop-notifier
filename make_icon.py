#!/usr/bin/env python3
"""Generate app.ico (16/32/48 px) with a green crescent-and-dot mark. Pure Python, no deps."""
import struct, zlib, math

def render(size):
    px = [[(0, 0, 0, 0) for _ in range(size)] for _ in range(size)]
    c = (size - 1) / 2.0
    R = size * 0.46
    for y in range(size):
        for x in range(size):
            d = math.hypot(x - c, y - c)
            if d <= R:
                # dark green disc with subtle radial shading
                t = d / R
                g = int(150 - 60 * t)
                px[y][x] = (0, g, int(40 - 20 * t), 255)
    # crescent: white circle minus offset circle
    r1 = size * 0.28; r2 = size * 0.24
    cx1, cy1 = c - size * 0.04, c
    cx2, cy2 = c + size * 0.06, c - size * 0.03
    for y in range(size):
        for x in range(size):
            if math.hypot(x - cx1, y - cy1) <= r1 and math.hypot(x - cx2, y - cy2) > r2:
                px[y][x] = (255, 255, 255, 255)
    # small dot (star)
    sx, sy = c + size * 0.2, c - size * 0.18
    for y in range(size):
        for x in range(size):
            if math.hypot(x - sx, y - sy) <= size * 0.07:
                px[y][x] = (255, 225, 120, 255)
    return px

def png(px):
    size = len(px)
    raw = b''.join(b'\x00' + b''.join(struct.pack('BBBB', *p) for p in row) for row in px)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

sizes = [16, 32, 48, 256]
imgs = [png(render(s)) for s in sizes]
hdr = struct.pack('<HHH', 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
entries = b''
for s, im in zip(sizes, imgs):
    entries += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(im), off)
    off += len(im)
with open('app.ico', 'wb') as f:
    f.write(hdr + entries + b''.join(imgs))
print('app.ico written')
