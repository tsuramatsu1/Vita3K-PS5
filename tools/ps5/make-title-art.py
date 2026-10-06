#!/usr/bin/env python3
# Vita3K - the PS5 title's launch art (vita3k/ps5/sce_sys/pic0.dds, pic1.dds) and the front end's splash
# (vita3k/ps5/assets/home/splash.png).
#
# The picture is the Vita3K logo (vita3k/Vita3K.png) and its name on a dark blue sky with a lit wave,
# 3840x2160, in the format the PS5 launcher reads: DDS with a DX10 header, BC7_UNORM,
# one level. pic1 is the splash the shell shows while the title starts; pic0, the same picture, is the background
# behind it on the home screen. The front end shows splash.png, the same picture at 1920x1080, as it opens.
#
# The BC7 encoder and the DDS header are PS5 RetroArch's tools/make-title-art.py (Mihawk-99,
# GPL-3.0-or-later): mode 6 blocks from a principal axis and one least-squares refit, so no texture compressor is
# needed and a run gives the same files every time.
#
#   python3 tools/ps5/make-title-art.py            write pic0.dds, pic1.dds and splash.png
#   python3 tools/ps5/make-title-art.py --png DIR  also write a PNG preview to DIR
#
# SPDX-License-Identifier: GPL-3.0-or-later
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

WIDTH, HEIGHT = 3840, 2160
ROOT = Path(__file__).resolve().parent.parent.parent
LOGO = ROOT / "vita3k" / "Vita3K.png"
SCE_SYS = ROOT / "vita3k" / "ps5" / "sce_sys"
SPLASH = ROOT / "vita3k" / "ps5" / "assets" / "home" / "splash.png"
# The name under the logo; drawn only where this font is installed, as on the build host
FONT = Path("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")

LOGO_SIZE = 900
NAME_SIZE = 190


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def background(width=1920, height=1080) -> np.ndarray:
    y, x = np.mgrid[0:height, 0:width].astype(np.float64)
    u = x / width
    v = y / height

    top = np.array([1.0, 2.0, 8.0])
    bottom = np.array([10.0, 26.0, 92.0])
    img = top + (bottom - top) * (v[..., None] ** 1.6)

    # The wave: a gentle curve, lower at the left, that the lit sheet of blue sits under
    crest = 0.50 - 0.09 * u + 0.025 * np.sin(u * np.pi * 1.3 + 0.4)
    below = v - crest
    sheet = smoothstep(0.0, 0.015, below) * np.exp(-np.clip(below, 0.0, None) / 0.32)
    img += sheet[..., None] * np.array([22.0, 58.0, 150.0]) * (0.55 + 0.45 * u)[..., None]

    # The bright edge along the crest, strongest towards the right, and its haze above
    edge = np.exp(-((below / 0.006) ** 2)) * (0.35 + 0.65 * smoothstep(0.1, 0.9, u))
    haze = np.exp(-((below / 0.05) ** 2)) * 0.35
    img += edge[..., None] * np.array([120.0, 170.0, 255.0]) + haze[..., None] * np.array([20.0, 45.0, 110.0])

    # A second, fainter fold further down
    crest2 = 0.74 - 0.05 * u + 0.02 * np.sin(u * np.pi * 2.0 + 1.2)
    fold = np.exp(-(((v - crest2) / 0.04) ** 2)) * 0.5
    img += fold[..., None] * np.array([14.0, 34.0, 90.0])

    # Darker towards the corners
    vignette = 1.0 - 0.35 * (((u - 0.5) * 2.0) ** 2) * (0.5 + 0.5 * v)
    return np.clip(img * vignette[..., None], 0, 255).astype(np.uint8)


def draw() -> np.ndarray:
    canvas = Image.fromarray(background(WIDTH, HEIGHT), "RGB").convert("RGBA")

    logo = Image.open(LOGO).convert("RGBA").resize((LOGO_SIZE, LOGO_SIZE), Image.LANCZOS)
    top, left = HEIGHT // 2 - LOGO_SIZE // 2 - 170, (WIDTH - LOGO_SIZE) // 2

    # A soft light behind the logo, so it stands off the dark sky
    glow = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((left - 160, top - 160, left + LOGO_SIZE + 160, top + LOGO_SIZE + 160), fill=(120, 170, 255, 70))
    canvas.alpha_composite(glow.filter(ImageFilter.GaussianBlur(140)))
    canvas.alpha_composite(logo, (left, top))

    if FONT.exists():
        font = ImageFont.truetype(str(FONT), NAME_SIZE)
        text = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
        pen = ImageDraw.Draw(text)
        y = top + LOGO_SIZE + 60
        pen.text((WIDTH // 2 + 6, y + 8), "Vita3K", font=font, fill=(0, 0, 0, 150), anchor="mt")
        canvas.alpha_composite(text.filter(ImageFilter.GaussianBlur(8)))
        ImageDraw.Draw(canvas).text((WIDTH // 2, y), "Vita3K", font=font, fill=(255, 255, 255, 255), anchor="mt")

    return np.asarray(canvas.convert("RGB")).astype(np.float64)

WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], dtype=np.int64)


def quantise(e: np.ndarray) -> np.ndarray:
    """Endpoint colours to the 7 stored bits (the p-bit is 1, for alpha 255)."""
    return np.clip(np.round((e - 1.0) / 2.0), 0, 127).astype(np.int64)


def palette(q0: np.ndarray, q1: np.ndarray) -> np.ndarray:
    """The 16 colours two quantised endpoints give, (blocks, 16, 3)."""
    e0, e1 = (q0 * 2 + 1)[:, None, :], (q1 * 2 + 1)[:, None, :]
    w = WEIGHTS[None, :, None]
    return ((64 - w) * e0 + w * e1 + 32) >> 6


def choose(px: np.ndarray, q0: np.ndarray, q1: np.ndarray):
    """Each pixel's best index for these endpoints, and the block's error."""
    pal = palette(q0, q1)
    err = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)
    idx = err.argmin(-1)
    return idx, np.take_along_axis(err, idx[..., None], -1)[..., 0].sum(-1)


def encode_blocks(px: np.ndarray) -> bytes:
    """BC7 mode 6 for blocks of 16 RGB pixels (blocks, 16, 3), alpha 255."""
    pf = px.astype(np.float64)
    mean = pf.mean(1)
    cen = pf - mean[:, None, :]
    cov = np.einsum('nki,nkj->nij', cen, cen)
    axis = np.ones((len(px), 3)) / np.sqrt(3.0)
    for _ in range(8):
        axis = np.einsum('nij,nj->ni', cov, axis)
        norm = np.sqrt((axis * axis).sum(1, keepdims=True))
        axis = np.where(norm > 1e-9, axis / np.maximum(norm, 1e-9), 1.0 / np.sqrt(3.0))
    proj = np.einsum('nki,ni->nk', cen, axis)
    q0 = quantise(mean + proj.min(1, keepdims=True) * axis)
    q1 = quantise(mean + proj.max(1, keepdims=True) * axis)
    idx, err = choose(px, q0, q1)
    # One least-squares refit of the endpoints to the chosen indices
    w = WEIGHTS[idx].astype(np.float64) / 64.0
    a, b, c = ((1 - w) ** 2).sum(1), ((1 - w) * w).sum(1), (w * w).sum(1)
    d0 = ((1 - w)[..., None] * pf).sum(1)
    d1 = (w[..., None] * pf).sum(1)
    det = a * c - b * b
    ok = det > 1e-9
    safe = np.where(ok, det, 1.0)[:, None]
    r0 = quantise(np.where(ok[:, None], (c[:, None] * d0 - b[:, None] * d1) / safe, mean))
    r1 = quantise(np.where(ok[:, None], (a[:, None] * d1 - b[:, None] * d0) / safe, mean))
    idx2, err2 = choose(px, r0, r1)
    better = (err2 < err)[:, None]
    q0, q1 = np.where(better, r0, q0), np.where(better, r1, q1)
    idx = np.where(better, idx2, idx)
    # The first pixel's index is stored in 3 bits: swap the ends if it needs 4
    swap = idx[:, 0] >= 8
    q0, q1 = np.where(swap[:, None], q1, q0), np.where(swap[:, None], q0, q1)
    idx = np.where(swap[:, None], 15 - idx, idx)

    lo = np.full(len(px), 1 << 6, dtype=np.uint64)   # mode 6
    hi = np.zeros(len(px), dtype=np.uint64)
    pos = 7

    def put(value: np.ndarray, bits: int):
        nonlocal pos
        for bit in range(bits):
            v = ((value >> bit) & 1).astype(np.uint64)
            if pos < 64:
                lo[:] |= v << np.uint64(pos)
            else:
                hi[:] |= v << np.uint64(pos - 64)
            pos += 1

    for ch in range(3):
        put(q0[:, ch], 7)
        put(q1[:, ch], 7)
    alpha = np.full(len(px), 127, dtype=np.int64)
    put(alpha, 7)
    put(alpha, 7)
    one = np.ones(len(px), dtype=np.int64)
    put(one, 1)   # P0
    put(one, 1)   # P1
    put(idx[:, 0], 3)
    for i in range(1, 16):
        put(idx[:, i], 4)
    out = np.empty(len(px) * 2, dtype="<u8")
    out[0::2], out[1::2] = lo, hi
    return out.tobytes()


def bc7(img: np.ndarray) -> bytes:
    """The picture as BC7 blocks, row of blocks by row of blocks."""
    rgb = np.clip(np.round(img), 0, 255).astype(np.int64)
    by, bx = HEIGHT // 4, WIDTH // 4
    blocks = rgb.reshape(by, 4, bx, 4, 3).transpose(0, 2, 1, 3, 4).reshape(by * bx, 16, 3)
    step = bx * 16
    return b"".join(encode_blocks(blocks[i:i + step]) for i in range(0, len(blocks), step))


def dds_header() -> bytes:
    """The DDS header of the launcher art the console has shown: DX10, BC7_UNORM
    (98), 2D, one level, straight alpha."""
    DDSD_CAPS, DDSD_HEIGHT, DDSD_WIDTH, DDSD_PIXELFORMAT = 0x1, 0x2, 0x4, 0x1000
    DDSD_MIPMAPCOUNT, DDSD_LINEARSIZE = 0x20000, 0x80000
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE
    linear = WIDTH * HEIGHT               # BC7: 16 bytes a 4x4 block, one byte a pixel
    header = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, HEIGHT, WIDTH, linear, 1, 1)
    header += b"\0" * 44                  # reserved
    header += struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)   # caps: texture
    header += struct.pack("<IIIII", 98, 3, 0, 1, 1)       # BC7_UNORM, TEXTURE2D, array 1, straight alpha
    assert len(header) == 148
    return header



def main() -> int:
    png_dir = None
    if len(sys.argv) == 3 and sys.argv[1] == "--png":
        png_dir = Path(sys.argv[2])
    elif len(sys.argv) != 1:
        print("usage: make-title-art.py [--png DIR]", file=sys.stderr)
        return 2
    img = draw()
    data = dds_header() + bc7(img)
    for name in ("pic0.dds", "pic1.dds"):
        (SCE_SYS / name).write_bytes(data)
        print(f"vita3k/ps5/sce_sys/{name}: {len(data)} bytes")
    picture = Image.fromarray(np.clip(np.round(img), 0, 255).astype(np.uint8), "RGB")
    SPLASH.parent.mkdir(parents=True, exist_ok=True)
    picture.resize((WIDTH // 2, HEIGHT // 2), Image.LANCZOS).save(SPLASH, optimize=True)
    print(f"vita3k/ps5/assets/home/{SPLASH.name}")
    if png_dir:
        png_dir.mkdir(parents=True, exist_ok=True)
        picture.save(png_dir / "title-art.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
