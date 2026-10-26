#!/usr/bin/env python3
# Generates the LiveArea images of every sample into samples/sce_sys/<sample>/.
# Needs Pillow and the DejaVu fonts. Run from the repository root.
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_DIR = "/usr/share/fonts/truetype/dejavu"
BG = (18, 20, 28)
GRID = (30, 34, 46)
TEXT = (236, 238, 244)
DIM = (150, 156, 172)
SS = 4  # supersampling factor for smooth edges

SAMPLES = {
    "hello_triangle": dict(label="Triangle", accent=(232, 85, 63), motif="triangle",
                           tagline="One triangle, the hello world of Vulkan"),
    "textured_cube": dict(label="Cube", accent=(64, 140, 235), motif="tex_cube",
                          tagline="Staging uploads, push constants and depth testing"),
    "fixed_function": dict(label="FixedFn", accent=(168, 92, 230), motif="fixed",
                           tagline="OpenGL 1.x style drawing inside a Vulkan render pass"),
    "spirv_cube": dict(label="SPIR-V", accent=(70, 200, 120), motif="wire_cube",
                       tagline="GLSL compiled to SPIR-V, translated at runtime"),
    "gpu_features": dict(label="Features", accent=(240, 180, 40), motif="panels",
                         tagline="Mipmaps, cube maps, queries, lines and more"),
}


def font(size, bold=True):
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    return ImageFont.truetype(os.path.join(FONT_DIR, name), size * SS)


def shade(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c)


def canvas(w, h, grid):
    im = Image.new("RGB", (w * SS, h * SS), BG)
    d = ImageDraw.Draw(im)
    if grid:
        for x in range(0, w, grid):
            d.line([(x * SS, 0), (x * SS, h * SS)], fill=GRID, width=SS)
        for y in range(0, h, grid):
            d.line([(0, y * SS), (w * SS, y * SS)], fill=GRID, width=SS)
    return im, d


def iso_cube(cx, cy, r):
    """Corner points of an isometric cube centered at (cx, cy) with edge r."""
    dx, dy = r * math.cos(math.radians(30)), r * 0.5
    top = (cx, cy - r)
    return dict(
        top=top, tl=(cx - dx, cy - dy), tr=(cx + dx, cy - dy), c=(cx, cy),
        bl=(cx - dx, cy + dy), br=(cx + dx, cy + dy), bottom=(cx, cy + r),
    )


def lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def quad_point(p0, p1, p3, u, v):
    # p0 origin, p1 along u, p3 along v
    return (p0[0] + (p1[0] - p0[0]) * u + (p3[0] - p0[0]) * v,
            p0[1] + (p1[1] - p0[1]) * u + (p3[1] - p0[1]) * v)


def checker_face(d, p0, p1, p3, ca, cb, n=4):
    for i in range(n):
        for j in range(n):
            pts = [quad_point(p0, p1, p3, i / n, j / n), quad_point(p0, p1, p3, (i + 1) / n, j / n),
                   quad_point(p0, p1, p3, (i + 1) / n, (j + 1) / n), quad_point(p0, p1, p3, i / n, (j + 1) / n)]
            d.polygon(pts, fill=ca if (i + j) % 2 == 0 else cb)


def draw_motif(d, kind, accent, cx, cy, size):
    """Draws the motif in supersampled coordinates; size is the half extent."""
    if kind == "triangle":
        h = size * 1.7
        d.polygon([(cx, cy - h * 0.55), (cx - size, cy + h * 0.45), (cx + size, cy + h * 0.45)], fill=accent)
    elif kind == "tex_cube":
        q = iso_cube(cx, cy, size)
        checker_face(d, q["tl"], q["top"], q["c"], shade(accent, 1.15), shade(accent, 0.85))
        checker_face(d, q["tl"], q["c"], q["bl"], shade(accent, 0.75), shade(accent, 0.55))
        checker_face(d, q["c"], q["tr"], q["bottom"], shade(accent, 0.95), shade(accent, 0.7))
    elif kind == "wire_cube":
        q = iso_cube(cx, cy, size)
        d.polygon([q["top"], q["tr"], q["c"], q["tl"]], fill=shade(accent, 0.45))
        d.polygon([q["tl"], q["c"], q["bottom"], q["bl"]], fill=shade(accent, 0.3))
        d.polygon([q["c"], q["tr"], q["br"], q["bottom"]], fill=shade(accent, 0.38))
        w = max(SS, int(size * 0.07))
        edges = [("top", "tr"), ("tr", "br"), ("br", "bottom"), ("bottom", "bl"), ("bl", "tl"), ("tl", "top"),
                 ("c", "tl"), ("c", "tr"), ("c", "bottom")]
        for a, b in edges:
            d.line([q[a], q[b]], fill=accent, width=w)
        for x, y in q.values():
            rr = w * 1.1
            d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=accent)
    elif kind == "fixed":
        # three overlapping translucent squares: blending
        s = size * 0.95
        layers = [(-0.45, -0.35, shade(accent, 1.2)), (0.0, 0.15, accent), (0.45, -0.1, shade(accent, 0.6))]
        img = d._image
        for ox, oy, col in layers:
            over = Image.new("RGBA", img.size, (0, 0, 0, 0))
            od = ImageDraw.Draw(over)
            x0, y0 = cx + ox * size - s * 0.6, cy + oy * size - s * 0.6
            od.rounded_rectangle([x0, y0, x0 + s * 1.2, y0 + s * 1.2], radius=s * 0.12, fill=col + (190,))
            img.paste(Image.alpha_composite(img.convert("RGBA"), over).convert("RGB"))
    elif kind == "panels":
        g = size * 0.08
        cell = size - g / 2
        cols = [accent, shade(accent, 0.75), shade(accent, 0.55), shade(accent, 0.9)]
        k = 0
        for j in range(2):
            for i in range(2):
                x0 = cx - size + i * (cell + g)
                y0 = cy - size + j * (cell + g)
                d.rounded_rectangle([x0, y0, x0 + cell, y0 + cell], radius=cell * 0.1, fill=shade(cols[k], 0.35),
                                    outline=cols[k], width=max(SS, int(size * 0.035)))
                k += 1
        # a little cube in panel 0, mip bands in 1, lines in 2, a dot grid in 3
        c0 = (cx - size + cell / 2, cy - size + cell / 2)
        q = iso_cube(c0[0], c0[1], cell * 0.3)
        d.polygon([q["top"], q["tr"], q["c"], q["tl"]], fill=shade(accent, 1.2))
        d.polygon([q["tl"], q["c"], q["bottom"], q["bl"]], fill=shade(accent, 0.7))
        d.polygon([q["c"], q["tr"], q["br"], q["bottom"]], fill=shade(accent, 0.9))
        x1 = cx + g / 2
        for b in range(4):
            bw = cell / 4
            d.rectangle([x1 + b * bw + cell * 0.05, cy - size + cell * 0.25,
                         x1 + (b + 1) * bw - cell * 0.02, cy - size + cell * 0.75],
                        fill=shade(accent, 1.0 - b * 0.18))
        y2 = cy + g / 2
        for t in range(3):
            d.line([(cx - size + cell * 0.15, y2 + cell * (0.25 + t * 0.25)),
                    (cx - size + cell * 0.85, y2 + cell * (0.75 - t * 0.2))],
                   fill=shade(accent, 1.15), width=max(SS, int(cell * (0.03 + t * 0.03))))
        for a in range(3):
            for b in range(3):
                x = x1 + cell * (0.25 + a * 0.25)
                y = y2 + cell * (0.25 + b * 0.25)
                rr = cell * 0.06
                d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=shade(accent, 1.1))


def text_center(d, xy, s, f, fill):
    x, y = xy
    bb = d.textbbox((0, 0), s, font=f)
    d.text((x - (bb[2] - bb[0]) / 2 - bb[0], y - (bb[3] - bb[1]) / 2 - bb[1]), s, font=f, fill=fill)


def finish(im, w, h, path):
    out = im.resize((w, h), Image.LANCZOS).convert("P", palette=Image.ADAPTIVE, colors=256)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    out.save(path, optimize=True)


TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<livearea style="a1" format-ver="01.00" content-rev="1">
  <livearea-background>
    <image>bg.png</image>
  </livearea-background>
  <gate>
    <startup-image>startup.png</startup-image>
  </gate>
</livearea>
"""


def build(root, name, cfg):
    base = os.path.join(root, "samples", "sce_sys", name)
    acc = cfg["accent"]

    # icon0.png 128x128
    im, d = canvas(128, 128, 0)
    d.rectangle([0, 0, 128 * SS, 5 * SS], fill=acc)
    draw_motif(d, cfg["motif"], acc, 64 * SS, 52 * SS, 30 * SS)
    text_center(d, (64 * SS, 107 * SS), cfg["label"], font(17), TEXT)
    finish(im, 128, 128, os.path.join(base, "icon0.png"))

    # bg.png 840x500
    im, d = canvas(840, 500, 40)
    d.rectangle([0, 0, 840 * SS, 6 * SS], fill=acc)
    draw_motif(d, cfg["motif"], acc, 420 * SS, 200 * SS, 105 * SS)
    text_center(d, (420 * SS, 385 * SS), "VkVita " + cfg["label"], font(38), TEXT)
    text_center(d, (420 * SS, 435 * SS), cfg["tagline"], font(22, bold=False), DIM)
    finish(im, 840, 500, os.path.join(base, "livearea", "contents", "bg.png"))

    # startup.png 280x158
    im, d = canvas(280, 158, 0)
    d.rectangle([0, 0, 280 * SS, 158 * SS], fill=shade(acc, 0.22))
    d.rectangle([0, 152 * SS, 280 * SS, 158 * SS], fill=acc)
    draw_motif(d, cfg["motif"], acc, 62 * SS, 76 * SS, 36 * SS)
    f = font(24)
    d.text((118 * SS, 46 * SS), "VkVita", font=f, fill=DIM)
    d.text((118 * SS, 78 * SS), cfg["label"], font=f, fill=TEXT)
    finish(im, 280, 158, os.path.join(base, "livearea", "contents", "startup.png"))

    with open(os.path.join(base, "livearea", "contents", "template.xml"), "w", newline="\n") as fp:
        fp.write(TEMPLATE)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    for name, cfg in SAMPLES.items():
        build(root, name, cfg)
        print("wrote", name)


if __name__ == "__main__":
    main()
