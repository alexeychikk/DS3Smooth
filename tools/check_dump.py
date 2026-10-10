"""Checks a DLAA dump (dlaa_dump = 1 or F10 in game; DS3DLAA\\dump\\) offline.

usage: python check_dump.py <dump dir> [evaluation number] [--png out_dir]

Prints the jitter/camera info, motion vector statistics (overall, object pixels, camera-only pixels)
and, when the next evaluation is also in the dump, how well the motion vectors explain the frame
change: frame N+1 is warped back to frame N with N+1's motion vectors and compared to frame N, against
not warping at all. With --png it also writes viewable PNGs (color, motion vectors, objects, output).
"""
import os
import struct
import sys

import numpy as np

DXGI = {9: (np.float16, 4), 10: (np.float16, 4), 16: (np.float32, 2), 34: (np.float16, 2), 41: (np.float32, 1)}


def read_dds(path):
    with open(path, "rb") as f:
        data = f.read()
    height, width = struct.unpack_from("<II", data, 12)
    fmt = struct.unpack_from("<I", data, 128)[0]
    dtype, channels = DXGI[fmt]
    pixels = np.frombuffer(data, dtype=dtype, offset=148, count=width * height * channels)
    return pixels.reshape(height, width, channels).astype(np.float32)


def warp(image, mv):
    """Samples image at (pixel + mv) with bilinear filtering: frame N+1 pulled back to frame N."""
    h, w = image.shape[:2]
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    x = np.clip(xs + mv[..., 0], 0, w - 1.001)
    y = np.clip(ys + mv[..., 1], 0, h - 1.001)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    a = image[y0, x0] * (1 - fx) + image[y0, x0 + 1] * fx
    b = image[y0 + 1, x0] * (1 - fx) + image[y0 + 1, x0 + 1] * fx
    return a * (1 - fy) + b * fy


def luminance(c):
    return 0.2126 * c[..., 0] + 0.7152 * c[..., 1] + 0.0722 * c[..., 2]


def stats(name, v):
    if v.size == 0:
        print(f"  {name}: none")
        return
    print(f"  {name}: {v.shape[0]} px, mean ({v[:, 0].mean():.3f}, {v[:, 1].mean():.3f}), "
          f"min ({v[:, 0].min():.2f}, {v[:, 1].min():.2f}), max ({v[:, 0].max():.2f}, {v[:, 1].max():.2f})")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    png = sys.argv[sys.argv.index("--png") + 1] if "--png" in sys.argv else None
    if png and png in args:
        args.remove(png)
    folder = args[0]
    numbers = sorted({int(n.split("_")[0]) for n in os.listdir(folder) if n.endswith("_mv.dds")})
    n = int(args[1]) if len(args) > 1 else numbers[0]
    p = lambda k, what: os.path.join(folder, f"{k}_{what}")
    print(open(p(n, "info.txt")).read().strip())
    mv = read_dds(p(n, "mv.dds"))
    objects = read_dds(p(n, "objects.dds"))
    depth = read_dds(p(n, "depth.dds"))[..., 0]
    is_object = objects[..., 0] >= 0
    print(f"motion vectors (pixels, current -> previous), {mv.shape[1]}x{mv.shape[0]}:")
    stats("all", mv.reshape(-1, 2))
    stats("objects", mv[is_object])
    stats("camera only", mv[~is_object])
    print(f"  depth: min {depth.min():.6f} max {depth.max():.6f}, sky (1.0) {np.mean(depth >= 1) * 100:.1f}%")

    if n + 1 in numbers and os.path.exists(p(n, "color.dds")) and os.path.exists(p(n + 1, "color.dds")):
        # A pixel's motion vector points to where it was: frame N sampled at (pixel + mv of N+1) should
        # look like frame N+1. Both frames are jittered differently, so even perfect vectors leave a
        # sub-pixel difference on sharp edges; what matters is warped << not warped.
        cur = read_dds(p(n + 1, "color.dds"))[..., :3]
        prev = read_dds(p(n, "color.dds"))[..., :3]
        mv1 = read_dds(p(n + 1, "mv.dds"))
        lc = np.log1p(np.maximum(luminance(cur), 0))
        still = np.abs(np.log1p(np.maximum(luminance(prev), 0)) - lc)
        warped = np.abs(np.log1p(np.maximum(luminance(warp(prev, mv1)), 0)) - lc)
        m = 8  # ignore the borders, where pixels come from outside the screen
        still, warped = still[m:-m, m:-m], warped[m:-m, m:-m]
        print(f"frame {n} -> {n + 1} difference (log luminance, mean): not warped {still.mean():.5f}, "
              f"warped with motion vectors {warped.mean():.5f}  ({'better' if warped.mean() < still.mean() else 'WORSE'})")

    if png:
        from PIL import Image
        os.makedirs(png, exist_ok=True)
        tonemap = lambda c: (np.clip(c / (1 + c), 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
        Image.fromarray(tonemap(read_dds(p(n, "color.dds"))[..., :3])).save(os.path.join(png, f"{n}_color.png"))
        Image.fromarray(tonemap(read_dds(p(n, "output.dds"))[..., :3])).save(os.path.join(png, f"{n}_output.png"))
        scale = 0.5 / max(np.percentile(np.abs(mv), 99), 0.5)
        vis = np.zeros(mv.shape[:2] + (3,), np.float32)
        vis[..., 0] = 0.5 + mv[..., 0] * scale
        vis[..., 1] = 0.5 + mv[..., 1] * scale
        vis[..., 2] = is_object * 1.0
        Image.fromarray((np.clip(vis, 0, 1) * 255).astype(np.uint8)).save(os.path.join(png, f"{n}_mv.png"))
        print(f"PNGs written to {png}")


if __name__ == "__main__":
    main()
