#!/usr/bin/env python3
"""[r]evo Slicer – generuje ikony, logo „O programie” i ekran startowy z logotypu Revo (gwiazdy-heksagony)."""
import io, math, sys
from pathlib import Path
from PIL import Image
import cairosvg
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

SRC = Path(sys.argv[1])            # revo-logo.png (białe „[r]evo3d”)
FONT = sys.argv[2]                 # BarlowSemiCondensed-SemiBold.ttf
OUT = Path(__file__).resolve().parents[2] / "resources/images"
GREEN, DARK, PANEL = "#3ed663", "#0d1014", "#1a1e24"

logo = Image.open(SRC).convert("RGBA")
A = logo.split()[3]


def dots(x0, x1, ox, oy, scale, step, fill, rmul=0.46):
    """Heksagony próbkowane z kanału alfa logo (kolumny x0..x1) – czysty wektor (nanosvg nie czyta obrazów)."""
    out = []
    for y in range(0, logo.height, step):
        for x in range(x0, x1, step):
            if A.getpixel((x, y)) > 120:
                cx, cy, r = ox + (x - x0) * scale, oy + y * scale, step * scale * rmul
                pts = " ".join(f"{cx + r * math.cos(math.pi / 3 * k + math.pi / 6):.2f},{cy + r * math.sin(math.pi / 3 * k + math.pi / 6):.2f}" for k in range(6))
                out.append(f'<polygon points="{pts}"/>')
    return f'<g fill="{fill}">' + "".join(out) + "</g>"


def text(s, x, y, size, fill, track=0.18):
    f = TTFont(FONT); gs = f.getGlyphSet(); cmap = f.getBestCmap(); upm = f["head"].unitsPerEm
    sc = size / upm; parts = []
    for ch in s:
        g = cmap[ord(ch)]
        pen = SVGPathPen(gs)
        gs[g].draw(TransformPen(pen, (sc, 0, 0, -sc, x, y)))
        parts.append(f'<path d="{pen.getCommands()}"/>')
        x += gs[g].width * sc + size * track
    return f'<g fill="{fill}">' + "".join(parts) + "</g>", x - size * track


def text_w(s, size, track=0.18):
    return text(s, 0, 0, size, "#000", track)[1]


def starfield(w, h, n, seed, avoid=None, rmax=1.6):
    """Tło galaktyki: rozsiane gwiazdy-heksagony o różnej jasności (bez filtrów – nanosvg ich nie obsługuje)."""
    import random
    rnd, out = random.Random(seed), []
    cx, cy = w / 2, h / 2
    for i in range(n):
        if rnd.random() < 0.6:   # ramiona spirali
            t = rnd.random() ** 0.7 * 3.4
            ang = rnd.choice([0, math.pi]) + t * 1.9 + rnd.gauss(0, 0.22)
            rr = t / 3.4 * w * 0.62 + rnd.gauss(0, w * 0.02)
            x, y = cx + rr * math.cos(ang), cy + rr * math.sin(ang) * 0.5
        else:
            x, y = rnd.uniform(0, w), rnd.uniform(0, h)
        if not (0 < x < w and 0 < y < h) or (avoid and avoid[0] < x < avoid[2] and avoid[1] < y < avoid[3]):
            continue
        r = rnd.choice([0.5, 0.7, 1.0, 1.3, rmax]) * w / 480
        pts = " ".join(f"{x + r * math.cos(math.pi / 3 * k + math.pi / 6):.1f},{y + r * math.sin(math.pi / 3 * k + math.pi / 6):.1f}" for k in range(6))
        out.append(f'<polygon points="{pts}" fill-opacity="{rnd.uniform(0.15, 0.7):.2f}"/>')
    return '<g fill="#dfe4ea">' + "".join(out) + "</g>"


def svg(w, h, body):
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">{body}</svg>'


R_END = 140   # „[r]” w logo (dalej przerwa, potem „evo”)
EVO_END = 418


def icon_svg(n=1024, step=6):
    s = n * 0.62 / R_END
    gw, gh = R_END * s, logo.height * s
    ox, oy = (n - gw) / 2, (n - gh) / 2 - n * 0.04
    body = f'<rect x="{n*0.04}" y="{n*0.04}" width="{n*0.92}" height="{n*0.92}" rx="{n*0.2}" fill="{DARK}" stroke="#2b3139" stroke-width="{n*0.012}"/>'
    if n >= 256:
        body += starfield(n, n, 260, 7, rmax=2.2).replace('<g fill', f'<g transform="translate({n*0.04} {n*0.04}) scale(0.92)" fill')
    body += dots(0, R_END, ox, oy, s, step, "#f0f3f6")
    body += f'<rect x="{n*0.36}" y="{n*0.76}" width="{n*0.28}" height="{n*0.028}" rx="{n*0.014}" fill="{GREEN}"/>'
    return svg(n, n, body)


def wordmark_svg(w, h, dot_fill, word_fill, step=3, pad=0.06):
    # „[r]evo” + „SLICER” w jednej linii, dopasowane do w×h
    fs = 0.62  # SLICER względem wysokości logo
    lw_unit = EVO_END + logo.height * fs * 0.45 + text_w("SLICER", logo.height * fs)
    s = min(w * (1 - 2 * pad) / lw_unit, h * (1 - 2 * pad) / logo.height)
    total = lw_unit * s
    ox, oy = (w - total) / 2, (h - logo.height * s) / 2
    body = dots(0, EVO_END, ox, oy, s, step, dot_fill)
    t, _ = text("SLICER", ox + (EVO_END + logo.height * fs * 0.45) * s, oy + logo.height * s * 0.5 + logo.height * fs * s * 0.35, logo.height * fs * s, word_fill)
    return svg(w, h, body + t)


def splash_svg(n, dark):
    bg = DARK if dark else "#f2f3f5"
    dot = "#f0f3f6" if dark else "#1a1e24"
    s = n * 0.72 / EVO_END
    ox, oy = (n - EVO_END * s) / 2, n * 0.30
    body = f'<rect width="{n}" height="{n}" rx="{n*0.06}" fill="{bg}"/>'
    body += starfield(n, n, 700, 1126) if dark else starfield(n, n, 500, 1126).replace("#dfe4ea", "#8a9099")
    body += dots(0, EVO_END, ox, oy, s, 3, dot)
    fs = n * 0.085; tw = text_w("SLICER", fs, 0.35)
    t, _ = text("SLICER", (n - tw) / 2, oy + logo.height * s + fs * 1.5, fs, "#9aa0a8" if dark else "#5a6068", 0.35)
    body += t + f'<rect x="{n*0.4}" y="{n*0.70}" width="{n*0.2}" height="{n*0.008}" fill="{GREEN}"/>'
    mt = "Sometimes evolution. Sometimes [r]evolution."
    ms = n * 0.03; mw = text_w(mt, ms, 0.02)
    m, _ = text(mt, (n - mw) / 2, n * 0.78, ms, "#6e747c" if dark else "#7a8088", 0.02)
    return svg(n, n, body + m)


def png(svgtxt, size):
    return Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svgtxt.encode(), output_width=size, output_height=size)))


def small_icon(size):
    """Małe ikony: pełny glif „[r]” zamiast kropek (kropki zlewają się poniżej 64 px)."""
    im = png(icon_svg(1024, 6), 1024)
    if size <= 64:
        base = Image.new("RGBA", (1024, 1024)); base.paste(png(icon_svg(1024, 6).split('<g fill')[0] + "</svg>", 1024))
        g = logo.crop((0, 0, R_END, logo.height)); s = 1024 * 0.62 / R_END
        g = g.resize((int(R_END * s), int(logo.height * s)), Image.LANCZOS)
        base.alpha_composite(g, (int((1024 - g.width) / 2), int((1024 - g.height) / 2 - 1024 * 0.04)))
        bar = png(icon_svg(1024, 6), 1024).crop((0, int(1024 * 0.7), 1024, 1024))
        base.alpha_composite(bar, (0, int(1024 * 0.7)))
        im = base
    return im.resize((size, size), Image.LANCZOS)


def main():
    W = lambda name, txt: (OUT / name).write_text(txt, encoding="utf-8")
    ic = icon_svg()
    for n in ("OrcaSlicer_gradient", "OrcaSlicer_gray", "OrcaSlicer_gradient_circle", "OrcaSlicer_gradient_narrow"):
        W(n + ".svg", ic)
    W("OrcaSlicer.svg", icon_svg(64, 9).replace('width="64" height="64"', 'width="64" height="64"'))
    W("OrcaSlicer_about.svg", wordmark_svg(560, 125, "#1a1e24", "#5a6068"))
    W("OrcaSlicer_about_dark.svg", wordmark_svg(560, 125, "#f0f3f6", "#9aa0a8"))
    W("OrcaSlicer_horizontal_light.svg", wordmark_svg(214, 80, "#1a1e24", "#5a6068", 4))
    W("OrcaSlicer_horizontal_dark.svg", wordmark_svg(214, 80, "#f0f3f6", "#9aa0a8", 4))
    W("splash_logo.svg", splash_svg(480, False))
    W("splash_logo_dark.svg", splash_svg(480, True))
    pngs = {"OrcaSlicer.png": 154, "OrcaSlicerTitle.png": 154, "OrcaSlicer_154.png": 154, "OrcaSlicer_154_title.png": 184,
            "OrcaSlicer_128px.png": 128, "OrcaSlicer-mac_128px.png": 128, "OrcaSlicer_192px.png": 192,
            "OrcaSlicer_192px_transparent.png": 192, "OrcaSlicer_32px.png": 32, "OrcaSlicer_64.png": 64}
    for f, n in pngs.items():
        small_icon(n).save(OUT / f)
    small_icon(192).convert("LA").convert("RGBA").save(OUT / "OrcaSlicer_192px_grayscale.png")
    big = small_icon(256)
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    ico = small_icon(256); ico.save(OUT / "OrcaSlicer.ico", sizes=sizes, append_images=[small_icon(s[0]) for s in sizes[:-1]])
    for f in ("OrcaSlicerTitle.ico", "OrcaSlicer-mac_256px.ico"):
        ico.save(OUT / f, sizes=sizes)
    small_icon(1024).save(OUT / "OrcaSlicer.icns")
    small_icon(1024).save(OUT.parent.parent / "tools/revo/revo-slicer-ikona-1024.png")
    print("ok")


if __name__ == "__main__":
    main()
