#!/usr/bin/env python3
"""Xbox and PlayStation button prompts for Age of Calamity, made from the player's own game files (runtime/prompts.c).

The game draws the button icons of its prompts and menus from one texture atlas (G1T 3226a433 in
asset/ScreenLayout.rdb.bin, 1024x1024 BC3 with one level), and the L and R tabs at the top of its menus from another
(G1T 8d1f8cb8, 512x256). This tool reads both from romfs.bin, draws Xbox or PlayStation buttons over the Switch
ones and writes four files next to romfs-index.bin (runtime/input_map.c input_prompts_folder):

    prompts-xbox.bin        Xbox, for the Nintendo pad layout (played by position: the Switch A is the right button,
                            labelled B on an Xbox pad): A shows B, B shows A, X shows Y and Y shows X
    prompts-xbox-label.bin  Xbox, for the Xbox, PlayStation and automatic layouts (A on the bottom button): A shows A
    prompts-ps.bin          PlayStation, Nintendo layout: A shows circle, B cross, X triangle, Y square
    prompts-ps-label.bin    PlayStation, the other layouts: A shows cross, B circle, X square, Y triangle

Xbox: L and R show LB and RB, ZL and ZR show LT and RT, + shows Menu (three bars) and - shows View (two squares).
PlayStation: L1, R1, L2, R2, + shows Options (three bars) and - shows Create (three slanted strokes).
The face buttons of the white icons take the controller's colours; the blue icons (the game's highlighted state)
keep the game's style of a symbol cut out of the button. The sticks, the d-pad and SL/SR stay as they are.

The drawings are made here, from lines, discs and rounded boxes (no font: the setup's Python has numpy but no
imaging library), anti-aliased by 4x4 supersampling. Only the BC3 blocks under the redrawn icons are encoded again.

File: header <8sII (magic "ACRPRM2\\0", texture count, 0), then per texture <IIQQ (width, height, FNV-1a 64 of the
game's BC3 blocks, size) and the new blocks in rows; the runtime swaps them in when the game uploads a texture with
the game's bytes.

    python3 tools/make_prompts.py --image build/v130/romfs.bin --out dist/windows [--png preview-folder]
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nx  # noqa: E402

CONTAINER = "asset/ScreenLayout.rdb.bin"
G1T_TYPE = 0xAFBEC60C
MAGIC = b"ACRPRM2\0"
# file: (controller, face buttons by position)
OUTPUTS = {"prompts-xbox.bin": ("xbox", True), "prompts-xbox-label.bin": ("xbox", False),
           "prompts-ps.bin": ("ps", True), "prompts-ps-label.bin": ("ps", False)}

WHITE, BLUE = (252, 252, 252), (60, 188, 252)
XBOX = {"A": (58, 154, 46), "B": (217, 50, 46), "X": (47, 111, 216), "Y": (209, 154, 0)}
PS = {"cross": (70, 120, 210), "circle": (214, 64, 72), "square": (200, 92, 168), "triangle": (32, 160, 140)}
# the Switch button -> what it shows: by label (A on the bottom button) and by position (A on the right one)
PS_LABEL = {"A": "cross", "B": "circle", "X": "square", "Y": "triangle"}
PS_POSITION = {"A": "circle", "B": "cross", "X": "triangle", "Y": "square"}
WORDS = {"xbox": {"LB": "LB", "RB": "RB", "LT": "LT", "RT": "RT"},
         "ps": {"LB": "L1", "RB": "R1", "LT": "L2", "RT": "R2"}}


# ---- the atlas in romfs.bin
def idrk_entries(blob: bytes):
    """(file KTID, type KTID, flags, stored bytes) of each IDRK entry of an RDB data file."""
    p = 0
    while p + 0x30 <= len(blob):
        if blob[p:p + 8] != b"IDRK0000":
            k = blob.find(b"IDRK0000", p + 1)
            if k < 0:
                return
            p = k
            continue
        esz, stored, _real = struct.unpack_from("<QQQ", blob, p + 8)
        _, fk, tk, flags = struct.unpack_from("<IIII", blob, p + 32)
        yield fk, tk, flags, blob[p + esz - stored:p + esz]
        p += esz if esz >= 0x30 else 8


def unpack(flags: int, data: bytes) -> bytes:
    """An entry's data: zlib blocks, each after its u32 size, when flag 0x100000 is set."""
    if not flags & 0x100000:
        return data
    out, p = bytearray(), 0
    while p + 4 <= len(data):
        n = struct.unpack_from("<I", data, p)[0]
        if not n:
            break
        out += zlib.decompress(data[p + 4:p + 4 + n])
        p += 4 + n
    return bytes(out)


def read_textures(image: Path) -> dict:
    """{name: BC3 blocks} of TEXTURES, each checked against the 1.3.0 game's."""
    with image.open("rb") as f:
        files = {name: (off, size) for name, off, size in nx.romfs_image_files(f)}
        if CONTAINER not in files:
            raise SystemExit(f"{image}: no {CONTAINER}")
        off, size = files[CONTAINER]
        f.seek(off)
        blob = f.read(size)
    by_ktid = {t["ktid"]: t for t in TEXTURES}
    found = {}
    for fk, tk, flags, data in idrk_entries(blob):
        t = by_ktid.get(fk)
        if not t or tk != G1T_TYPE:
            continue
        g = unpack(flags, data)
        magic, _ver, _size, toff, count = struct.unpack_from("<4s4sIII", g, 0)
        base = toff + struct.unpack_from("<I", g, toff + 4 * t["index"])[0]
        h = g[base:base + 8]
        ext = struct.unpack_from("<I", g, base + 8)[0] if h[7] & 0x11 else 0
        n = t["w"] * t["h"]   # BC3: a byte per texel
        blocks = g[base + 8 + ext:base + 8 + ext + n]
        dims = (t["w"].bit_length() - 1) | (t["h"].bit_length() - 1) << 4
        if magic != b"GT1G" or t["index"] >= count or h[1] != 0x5B or h[2] != dims or len(blocks) != n:
            raise SystemExit(f"{CONTAINER}: the {t['name']} texture is not the {t['w']}x{t['h']} BC3 one this tool knows")
        if hashlib.sha256(blocks).hexdigest() != t["sha256"]:
            raise SystemExit(f"{CONTAINER}: the {t['name']} texture differs from the 1.3.0 game's (a mod?); "
                             "prompts not made")
        found[t["name"]] = blocks
    missing = [t["name"] for t in TEXTURES if t["name"] not in found]
    if missing:
        raise SystemExit(f"{CONTAINER}: missing the {', '.join(missing)} texture")
    return found


def fnv1a64(b: bytes) -> int:
    """runtime/prompts.c prompts_hash (FNV-1a 64)."""
    h = 0xCBF29CE484222325
    for c in b:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# ---- BC3
def bc3_decode(blocks: bytes, w: int, h: int) -> np.ndarray:
    """RGBA uint8 (h, w, 4) of BC3 blocks in rows."""
    b = np.frombuffer(blocks, np.uint8).reshape(-1, 16)
    n = len(b)
    a0, a1 = b[:, 0].astype(np.int32), b[:, 1].astype(np.int32)
    pal8 = np.stack([a0, a1] + [((7 - k) * a0 + k * a1 + 3) // 7 for k in range(1, 7)], 1)
    pal6 = np.stack([a0, a1] + [((5 - k) * a0 + k * a1 + 2) // 5 for k in range(1, 5)] +
                    [np.zeros(n, np.int32), np.full(n, 255, np.int32)], 1)
    apal = np.where((a0 > a1)[:, None], pal8, pal6)
    abits = np.zeros(n, np.uint64)
    for k in range(6):
        abits |= b[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
    aidx = ((abits[:, None] >> (np.arange(16, dtype=np.uint64) * 3)[None]) & np.uint64(7)).astype(np.int64)
    alpha = np.take_along_axis(apal, aidx, 1)
    c0 = b[:, 8].astype(np.int32) | b[:, 9].astype(np.int32) << 8
    c1 = b[:, 10].astype(np.int32) | b[:, 11].astype(np.int32) << 8

    def rgb(c):
        r, g, bb = c >> 11 & 31, c >> 5 & 63, c & 31
        return np.stack([r << 3 | r >> 2, g << 2 | g >> 4, bb << 3 | bb >> 2], 1)
    e0, e1 = rgb(c0), rgb(c1)
    cpal = np.stack([e0, e1, (2 * e0 + e1) // 3, (e0 + 2 * e1) // 3], 1)   # BC3 colour is always four colours
    cbits = b[:, 12].astype(np.int64) | b[:, 13].astype(np.int64) << 8 | b[:, 14].astype(np.int64) << 16 | \
        b[:, 15].astype(np.int64) << 24
    cidx = (cbits[:, None] >> (np.arange(16) * 2)[None]) & 3
    col = np.take_along_axis(cpal, cidx[:, :, None].repeat(3, 2), 1)
    px = np.concatenate([col, alpha[:, :, None]], 2).astype(np.uint8)   # (n, 16, 4)
    return px.reshape(h // 4, w // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(h, w, 4)


def bc3_encode(px: np.ndarray) -> np.ndarray:
    """BC3 blocks (n, 16) uint8 of n blocks of 16 RGBA pixels (n, 16, 4): alpha between its min and max in eight
    steps; colour along the principal axis of the visible pixels."""
    n = len(px)
    a = px[:, :, 3].astype(np.int32)
    amax, amin = a.max(1), a.min(1)
    a0, a1 = amax, amin
    apal = np.stack([a0, a1] + [((7 - j) * a0 + j * a1 + 3) // 7 for j in range(1, 7)], 1)
    aidx = np.abs(a[:, :, None] - apal[:, None, :]).argmin(2)
    aidx = np.where((a0 == a1)[:, None], 0, aidx)
    out = np.zeros((n, 16), np.uint8)
    out[:, 0], out[:, 1] = a0, a1
    bits = np.zeros(n, np.uint64)
    for i in range(16):
        bits |= aidx[:, i].astype(np.uint64) << np.uint64(3 * i)
    for j in range(6):
        out[:, 2 + j] = ((bits >> np.uint64(8 * j)) & np.uint64(255)).astype(np.uint8)
    c = px[:, :, :3].astype(np.float64)
    w = (a > 8).astype(np.float64)
    w = np.where(w.sum(1, keepdims=True) > 0, w, 1.0)
    mean = (c * w[:, :, None]).sum(1) / w.sum(1)[:, None]
    d = (c - mean[:, None]) * w[:, :, None]
    cov = np.einsum("npi,npj->nij", d, c - mean[:, None])
    axis = np.ones((n, 3)) / np.sqrt(3)
    for _ in range(8):
        axis = np.einsum("nij,nj->ni", cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-9)
    t = np.einsum("npi,ni->np", c - mean[:, None], axis)
    big = 1e9
    tmin = np.where(w > 0, t, big).min(1)
    tmax = np.where(w > 0, t, -big).max(1)
    e0 = np.clip(mean + axis * tmax[:, None], 0, 255)
    e1 = np.clip(mean + axis * tmin[:, None], 0, 255)

    def q565(e):
        r = np.round(e[:, 0] * 31 / 255).astype(np.int32)
        g = np.round(e[:, 1] * 63 / 255).astype(np.int32)
        b = np.round(e[:, 2] * 31 / 255).astype(np.int32)
        return r << 11 | g << 5 | b

    def rgb(v):
        r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
        return np.stack([r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2], 1).astype(np.float64)
    c0, c1 = q565(e0), q565(e1)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    p0, p1 = rgb(c0), rgb(c1)
    cpal = np.stack([p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3], 1)
    cidx = ((c[:, :, None, :] - cpal[:, None, :, :]) ** 2).sum(3).argmin(2)
    out[:, 8], out[:, 9] = c0 & 255, c0 >> 8
    out[:, 10], out[:, 11] = c1 & 255, c1 >> 8
    cb = np.zeros(n, np.int64)
    for i in range(16):
        cb |= cidx[:, i].astype(np.int64) << (2 * i)
    for j in range(4):
        out[:, 12 + j] = (cb >> (8 * j)) & 255
    return out


# ---- drawing: signed distances (negative inside) sampled 4x4 per pixel
SS = 4


def grid(h: int, w: int):
    o = (np.arange(SS) + 0.5) / SS
    ys = (np.arange(h)[:, None] + o[None]).reshape(-1)
    xs = (np.arange(w)[:, None] + o[None]).reshape(-1)
    return np.meshgrid(ys, xs, indexing="ij")


def coverage(sdf: np.ndarray, h: int, w: int) -> np.ndarray:
    return (sdf < 0).reshape(h, SS, w, SS).mean((1, 3))


def sd_disc(y, x, cy, cx, r):
    return np.hypot(y - cy, x - cx) - r


def sd_box(y, x, cy, cx, hh, hw, r_top, r_bot=None):
    """Rounded box centred (cy, cx), half sizes hh, hw; corner radius r_top at the top, r_bot at the bottom."""
    r = np.where(y < cy, r_top, r_top if r_bot is None else r_bot)
    qy, qx = np.abs(y - cy) - hh + r, np.abs(x - cx) - hw + r
    return np.hypot(np.maximum(qy, 0), np.maximum(qx, 0)) + np.minimum(np.maximum(qy, qx), 0) - r


def sd_lines(y, x, strokes, half):
    """Distance to polylines [(y, x), ...] minus half the stroke width."""
    d = np.full(y.shape, np.inf)
    for pts in strokes:
        for (ay, ax), (by, bx) in zip(pts, pts[1:]):
            vy, vx = by - ay, bx - ax
            t = np.clip(((y - ay) * vy + (x - ax) * vx) / max(vy * vy + vx * vx, 1e-9), 0, 1)
            d = np.minimum(d, np.hypot(y - ay - t * vy, x - ax - t * vx))
    return d - half


def arc(cy, cx, r, a0, a1, n=10):
    return [(cy + r * np.sin(a), cx + r * np.cos(a)) for a in np.linspace(a0, a1, n)]


# letters in a box of height 1 and the given width; points (y, x)
P2 = np.pi / 2
GLYPHS = {
    "A": (0.86, [[(1, 0), (0, 0.43), (1, 0.86)], [(0.66, 0.19), (0.66, 0.67)]]),
    "B": (0.74, [[(0, 0), (1, 0)], [(0, 0)] + arc(0.235, 0.5, 0.235, -P2, P2) + [(0.47, 0)],
                 [(0.47, 0)] + arc(0.735, 0.48, 0.265, -P2, P2) + [(1, 0)]]),
    "X": (0.84, [[(0, 0), (1, 0.84)], [(0, 0.84), (1, 0)]]),
    "Y": (0.86, [[(0, 0), (0.5, 0.43), (0, 0.86)], [(0.5, 0.43), (1, 0.43)]]),
    "L": (0.66, [[(0, 0), (1, 0), (1, 0.66)]]),
    "R": (0.76, [[(1, 0), (0, 0)] + arc(0.25, 0.47, 0.25, -P2, P2) + [(0.5, 0)], [(0.5, 0.38), (1, 0.76)]]),
    "T": (0.78, [[(0, 0), (0, 0.78)], [(0, 0.39), (1, 0.39)]]),
    "1": (0.36, [[(0.22, 0), (0, 0.32), (1, 0.32)]]),
    "2": (0.68, [arc(0.3, 0.34, 0.3, np.pi, 2 * np.pi + 0.6) + [(1, 0), (1, 0.68)]]),
}


def sd_text(y, x, text, cy, cx, height, weight=0.19, gap=0.12):
    """Distance to a word of GLYPHS letters, centred on (cy, cx), cap height `height` (stroke included), `gap` of
    the height between letters."""
    t = height * weight
    inner = height - t
    widths = [GLYPHS[c][0] * inner for c in text]
    total = sum(widths) + (gap * height + t) * (len(text) - 1) + t
    left = cx - total / 2 + t / 2
    top = cy - height / 2 + t / 2
    strokes = []
    for c, wd in zip(text, widths):
        for pts in GLYPHS[c][1]:
            strokes.append([(top + py * inner, left + px * inner) for py, px in pts])
        left += wd + gap * height + t
    return sd_lines(y, x, strokes, t / 2)


def bars(y, x, cy, cx, size):
    """Menu: three horizontal bars."""
    hw, hh = size * 0.36, size * 0.065
    return np.minimum.reduce([sd_box(y, x, cy + k * size * 0.27, cx, hh, hw, hh) for k in (-1, 0, 1)])


def symbol(y, x, name, cy, cx, r):
    """A PlayStation face symbol, r the radius of the button."""
    t = r * 0.17   # stroke
    if name == "cross":
        k = r * 0.42
        return sd_lines(y, x, [[(cy - k, cx - k), (cy + k, cx + k)], [(cy - k, cx + k), (cy + k, cx - k)]], t / 2)
    if name == "circle":
        return np.abs(sd_disc(y, x, cy, cx, r * 0.46)) - t / 2
    if name == "square":
        k = r * 0.4
        return np.abs(sd_box(y, x, cy, cx, k, k, r * 0.05)) - t / 2
    k = r * 0.52   # triangle, centred on its centroid
    pts = [(cy - k, cx), (cy + k * 0.5, cx - k * 0.866), (cy + k * 0.5, cx + k * 0.866), (cy - k, cx)]
    return sd_lines(y, x, [pts], t / 2)


def strokes3(y, x, cy, cx, size):
    """Create: three short slanted strokes."""
    h, w = size * 0.3, size * 0.075
    return np.minimum.reduce([sd_lines(y, x, [[(cy - h, cx + k * size * 0.22 + h * 0.35),
                                              (cy + h, cx + k * size * 0.22 - h * 0.35)]], w) for k in (-1, 0, 1)])


def squares(y, x, cy, cx, size):
    """View: a square outline behind, up and left, and a filled square in front."""
    s, o = size * 0.24, size * 0.11
    back = np.abs(sd_box(y, x, cy - o, cx - o, s, s, s * 0.25)) - size * 0.045
    front = sd_box(y, x, cy + o, cx + o, s, s, s * 0.25)
    gap = sd_box(y, x, cy + o, cx + o, s + size * 0.07, s + size * 0.07, s * 0.35)
    return np.minimum(np.maximum(back, -gap), front)


# ---- the icons: (approximate box y0, x0, y1, x1 of the game's icon, what to draw)
FACE = [((404, 782, 455, 833), "A", WHITE), ((404, 835, 455, 886), "A", BLUE),
        ((404, 888, 455, 939), "B", WHITE), ((404, 941, 455, 992), "B", BLUE),
        ((669, 689, 720, 740), "X", WHITE), ((669, 742, 720, 793), "X", BLUE),
        ((669, 795, 720, 846), "Y", WHITE), ((669, 848, 720, 899), "Y", BLUE)]
SHOULDER = [((563, 373, 604, 422), "LB", WHITE), ((563, 426, 604, 475), "LB", BLUE),
            ((621, 530, 662, 579), "RB", WHITE), ((621, 583, 662, 632), "RB", BLUE)]
TRIGGER = [((566, 585, 611, 632), "LT", WHITE), ((566, 638, 611, 685), "LT", BLUE),
           ((619, 638, 664, 685), "RT", WHITE), ((619, 691, 664, 738), "RT", BLUE)]
ROUND = [((616, 164, 658, 206), "menu", WHITE), ((616, 217, 658, 259), "menu", BLUE),
         ((669, 217, 711, 259), "view", WHITE), ((669, 270, 711, 312), "view", BLUE)]
PLAIN = [((526, 481, 569, 524), "menu", WHITE), ((562, 4, 605, 47), "menu", BLUE),
         ((560, 55, 607, 102), "view", WHITE), ((560, 108, 607, 155), "view", BLUE)]
SWAP = {"A": "B", "B": "A", "X": "Y", "Y": "X"}


def tight(img: np.ndarray, box) -> tuple:
    """The box of the game's icon (alpha > 8) inside an approximate box."""
    y0, x0, y1, x1 = box
    ys, xs = np.nonzero(img[y0:y1 + 1, x0:x1 + 1, 3] > 8)
    if not len(ys):
        return box
    return y0 + ys.min(), x0 + xs.min(), y0 + ys.max(), x0 + xs.max()


def draw(img: np.ndarray, box, body, hole=None, ink=None, ink_color=None):
    """Clears box and draws a shape: body (sdf function of y, x) in its colour, minus hole, with ink painted on."""
    y0, x0, y1, x1 = box
    h, w = y1 - y0 + 1, x1 - x0 + 1
    y, x = grid(h, w)
    cov = coverage(body[0](y, x), h, w)
    if hole is not None:
        cov = cov * (1 - coverage(hole(y, x), h, w))
    rgb = np.empty((h, w, 3))
    rgb[:] = body[1]
    if ink is not None:
        k = coverage(ink(y, x), h, w)[:, :, None]
        rgb = rgb * (1 - k) + np.array(ink_color) * k
    img[y0:y1 + 1, x0:x1 + 1, :3] = np.round(rgb)
    img[y0:y1 + 1, x0:x1 + 1, 3] = np.round(cov * 255)


def redraw_buttons(img: np.ndarray, pad: str, by_position: bool) -> list:
    """Draws the Xbox or PlayStation icons into the button atlas (RGBA); returns the boxes it changed."""
    changed = []
    for box, letter, color in FACE:
        y0, x0, y1, x1 = tight(img, box)
        cy, cx, r = (y0 + y1 + 1) / 2 - box[0], (x0 + x1 + 1) / 2 - box[1], (y1 - y0 + 1) / 2
        disc = (lambda y, x, cy=cy, cx=cx, r=r: sd_disc(y, x, cy, cx, r), color)
        if pad == "xbox":
            show = SWAP[letter] if by_position else letter
            mark, ink = (lambda y, x, cy=cy, cx=cx, r=r, s=show: sd_text(y, x, s, cy, cx, r * 1.06)), XBOX[show]
        else:
            show = (PS_POSITION if by_position else PS_LABEL)[letter]
            mark, ink = (lambda y, x, cy=cy, cx=cx, r=r, s=show: symbol(y, x, s, cy, cx, r)), PS[show]
        if color == WHITE:
            draw(img, box, disc, ink=mark, ink_color=ink)
        else:
            draw(img, box, disc, hole=mark)
        changed.append(box)
    for group, top_r in ((SHOULDER, 0.5), (TRIGGER, 0.95)):
        for box, word, color in group:
            y0, x0, y1, x1 = tight(img, box)
            cy, cx = (y0 + y1 + 1) / 2 - box[0], (x0 + x1 + 1) / 2 - box[1]
            hh, hw = (y1 - y0 + 1) / 2, (x1 - x0 + 1) / 2
            if group is SHOULDER:   # a wide bar, rounded more at the top
                shape = lambda y, x, cy=cy, cx=cx, hh=hh, hw=hw: sd_box(y, x, cy, cx, hh * 0.86, hw, hh * 0.86, hh * 0.3)
                tcy = cy + hh * 0.06
            else:   # a trigger: a rounded top over a straight bottom
                shape = lambda y, x, cy=cy, cx=cx, hh=hh, hw=hw: sd_box(y, x, cy, cx, hh, hw * 0.9, hw * top_r, hh * 0.22)
                tcy = cy + hh * 0.2
            text = lambda y, x, cy=tcy, cx=cx, hh=hh, s=WORDS[pad][word]: sd_text(y, x, s, cy, cx, hh * 0.95)
            draw(img, box, (shape, color), hole=text)
            changed.append(box)
    for box, what, color in ROUND + PLAIN:
        y0, x0, y1, x1 = box
        cy, cx = (y1 - y0 + 1) / 2, (x1 - x0 + 1) / 2
        size = min(y1 - y0, x1 - x0) + 1
        mark = bars if what == "menu" else squares if pad == "xbox" else strokes3
        if (box, what, color) in ROUND:
            ty0, tx0, ty1, tx1 = tight(img, box)
            r = (ty1 - ty0 + 1) / 2
            tcy, tcx = (ty0 + ty1 + 1) / 2 - y0, (tx0 + tx1 + 1) / 2 - x0
            disc = (lambda y, x, cy=tcy, cx=tcx, r=r: sd_disc(y, x, cy, cx, r), color)
            draw(img, box, disc, hole=lambda y, x, cy=tcy, cx=tcx, r=r, m=mark: m(y, x, cy, cx, r * 1.25))
        else:
            draw(img, box, (lambda y, x, cy=cy, cx=cx, s=size * 0.9, m=mark: m(y, x, cy, cx, s), color))
        changed.append(box)
    return changed


def redraw_tabs(img: np.ndarray, pad: str, by_position: bool) -> list:
    """The menu's L and R tabs as LB and RB (L1 and R1)."""
    changed = []
    for box, word in TABS:
        y0, x0, y1, x1 = tight(img, box)
        cy, cx = (y0 + y1 + 1) / 2 - box[0], (x0 + x1 + 1) / 2 - box[1]
        hh, hw = (y1 - y0 + 1) / 2, (x1 - x0 + 1) / 2
        shape = lambda y, x, cy=cy, cx=cx, hh=hh, hw=hw: sd_box(y, x, cy, cx, hh * 0.9, hw, hh * 0.9, hh * 0.3)
        text = lambda y, x, cy=cy, cx=cx, hh=hh, s=WORDS[pad][word]: sd_text(y, x, s, cy, cx, hh * 1.0)
        draw(img, box, (shape, WHITE), hole=text)
        changed.append(box)
    return changed


TABS = [((4, 461, 58, 511), "LB"), ((58, 461, 110, 511), "RB")]
TEXTURES = [
    {"name": "button", "ktid": 0x3226A433, "index": 0, "w": 1024, "h": 1024, "redraw": redraw_buttons,
     "sha256": "2a23a3d28d5f37e98058337a19b88f75491cae84c09ec0bcc24bdfdb825112ba"},
    {"name": "menu tab", "ktid": 0x8D1F8CB8, "index": 0, "w": 512, "h": 256, "redraw": redraw_tabs,
     "sha256": "e35ed79c01580e1ef0644d298c8b19ed56282280e76dfd2eaa8b7e144532a4fb"},
]


def make(t: dict, blocks: bytes, pad: str, by_position: bool) -> tuple[bytes, np.ndarray]:
    """A texture's blocks with the pad's icons, and its RGBA for a preview."""
    w, h = t["w"], t["h"]
    img = bc3_decode(blocks, w, h).astype(np.float64)
    changed = t["redraw"](img, pad, by_position)
    img = np.clip(img, 0, 255).astype(np.uint8)
    out = np.frombuffer(blocks, np.uint8).reshape(h // 4, w // 4, 16).copy()
    todo = np.zeros((h // 4, w // 4), bool)
    for y0, x0, y1, x1 in changed:
        todo[y0 // 4:y1 // 4 + 1, x0 // 4:x1 // 4 + 1] = True
    by, bx = np.nonzero(todo)
    px = img.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4)[by, bx].reshape(-1, 16, 4)
    out[by, bx] = bc3_encode(px)
    return out.tobytes(), img


def write_png(path: Path, img: np.ndarray) -> None:
    """A preview PNG (RGBA 8-bit), standard library only."""
    raw = b"".join(b"\0" + img[y].tobytes() for y in range(img.shape[0]))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", img.shape[1], img.shape[0], 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--image", type=Path, required=True, help="romfs.bin of the 1.3.0 game")
    ap.add_argument("--out", type=Path, required=True, help="the folder of romfs-index.bin")
    ap.add_argument("--png", type=Path, help="also write previews of the textures here")
    args = ap.parse_args()
    game = read_textures(args.image)
    hashes = {t["name"]: fnv1a64(game[t["name"]]) for t in TEXTURES}
    for name, (pad, by_position) in OUTPUTS.items():
        new = struct.pack("<8sII", MAGIC, len(TEXTURES), 0)
        for t in TEXTURES:
            data, img = make(t, game[t["name"]], pad, by_position)
            new += struct.pack("<IIQQ", t["w"], t["h"], hashes[t["name"]], len(data)) + data
            if args.png:
                args.png.mkdir(parents=True, exist_ok=True)
                write_png(args.png / f"{Path(name).stem}-{t['name'].replace(' ', '-')}.png", img)
        dst = args.out / name
        if not dst.is_file() or dst.read_bytes() != new:
            tmp = dst.with_suffix(".tmp")
            tmp.write_bytes(new)
            tmp.replace(dst)
        print(f"{dst}: {len(new)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
