"""Redraws the dumped game pictures with Stable Diffusion XL, which adds detail instead of only enlarging.

The game saves each picture it shows into textures/dump when it runs with --dump-textures (see README). For every
dumped picture this tool takes a clean four-times enlargement (the pack picture that make-texture-pack.ps1 made
with Real-ESRGAN, or a new one through Upscayl), lets SDXL with the tile ControlNet repaint it at that size, keeps
the broad colours and shading of the clean enlargement with the fine detail of the repainted one, and puts the
result into textures/pack, where a running game picks it up within a few seconds. Large pictures, such as the
asteroid surface, are repainted in overlapping tiles of 1024 pixels.

It needs a Python with PyTorch (CUDA), diffusers, transformers, accelerate, numpy and Pillow, and the models
stabilityai/stable-diffusion-xl-base-1.0 (fp16), xinsir/controlnet-tile-sdxl-1.0 and madebyollin/sdxl-vae-fp16-fix
in the Hugging Face cache that HF_HOME names. With --watch it keeps running and repaints new dumps as they appear,
so each new asteroid's surface is redrawn while the game is played.

    python -I tools/redraw-textures.py --watch
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

import numpy as np
from PIL import Image, ImageFilter

MARGIN = 8  # transparent pixels around every dumped picture, see runtime/textures.h
SCALE = 4
TILE = 1024
OVERLAP = 256
PROMPT = ("highly detailed science fiction game art, 1990s pre-rendered CGI, crisp machined metal, sharp clean edges, "
          "fine surface detail, realistic lighting")
TERRAIN_PROMPT = ("highly detailed rocky asteroid surface, craters, boulders, fine rock and dust texture, realistic "
                  "lighting, sharp focus")
NEGATIVE = "blurry, soft, noise, jpeg artifacts, text, watermark, signature, people, faces, cartoon, oversaturated"


def default_folder():
    return os.path.join(os.environ.get("LOCALAPPDATA", ""), "Nax Fragile Allegiance", "textures")


def load_pipeline():
    import torch
    from diffusers import AutoencoderKL, ControlNetModel, StableDiffusionXLControlNetImg2ImgPipeline

    controlnet = ControlNetModel.from_pretrained("xinsir/controlnet-tile-sdxl-1.0", torch_dtype=torch.float16)
    vae = AutoencoderKL.from_pretrained("madebyollin/sdxl-vae-fp16-fix", torch_dtype=torch.float16)
    pipe = StableDiffusionXLControlNetImg2ImgPipeline.from_pretrained(
        "stabilityai/stable-diffusion-xl-base-1.0", controlnet=controlnet, vae=vae, torch_dtype=torch.float16,
        variant="fp16", use_safetensors=True)
    pipe.to("cuda")
    pipe.set_progress_bar_config(disable=True)
    return pipe


def clean_enlargement(dump_file, pack_file, upscayl, work):
    """The picture four times as large without new detail: the Real-ESRGAN pack picture, or a new one."""
    if os.path.exists(pack_file) and not os.path.exists(marker(pack_file)):
        return Image.open(pack_file).convert("RGBA")
    tool = os.path.join(upscayl, "bin", "upscayl-bin.exe")
    if os.path.exists(tool):
        out = os.path.join(work, os.path.basename(dump_file))
        ran = subprocess.run([tool, "-i", dump_file, "-o", out, "-m", os.path.join(upscayl, "models"), "-n", "high-fidelity-4x",
                              "-s", "4", "-f", "png"], capture_output=True, text=True, errors="replace")
        if ran.returncode == 0 and "failed" not in ran.stdout + ran.stderr and os.path.exists(out):
            return Image.open(out).convert("RGBA")
    small = Image.open(dump_file).convert("RGBA")
    return small.resize((small.width * SCALE, small.height * SCALE), Image.LANCZOS)


def marker(pack_file):
    folder, name = os.path.split(pack_file)
    return os.path.join(folder, "redrawn", os.path.splitext(name)[0])


def tile_weights(w, h, left, top, right, bottom):
    """Weights that fade in over the overlap on every side that meets another tile."""
    wx = np.ones(w, np.float32)
    wy = np.ones(h, np.float32)
    ramp = np.linspace(0.0, 1.0, OVERLAP, dtype=np.float32)
    if left:
        wx[:OVERLAP] = ramp
    if right:
        wx[-OVERLAP:] = np.minimum(wx[-OVERLAP:], ramp[::-1])
    if top:
        wy[:OVERLAP] = ramp
    if bottom:
        wy[-OVERLAP:] = np.minimum(wy[-OVERLAP:], ramp[::-1])
    return np.outer(wy, wx)[:, :, None]


def repaint(pipe, rgb, args, prompt, seed):
    """SDXL over the whole RGB picture, in overlapping tiles when it is larger than one tile."""
    import torch

    h, w = rgb.shape[:2]
    # Small pictures are repainted at the model's own size and brought back afterwards.
    k = max(1.0, TILE / max(w, h)) if max(w, h) < TILE else 1.0
    cw, ch = max(64, int(round(w * k / 8)) * 8), max(64, int(round(h * k / 8)) * 8)
    canvas = np.asarray(Image.fromarray(rgb).resize((cw, ch), Image.LANCZOS))
    xs = [0] if cw <= TILE else list(range(0, cw - TILE, TILE - OVERLAP)) + [cw - TILE]
    ys = [0] if ch <= TILE else list(range(0, ch - TILE, TILE - OVERLAP)) + [ch - TILE]
    total = np.zeros((ch, cw, 3), np.float32)
    weight = np.zeros((ch, cw, 1), np.float32)
    for y in ys:
        for x in xs:
            tw, th = min(TILE, cw), min(TILE, ch)
            piece = Image.fromarray(canvas[y:y + th, x:x + tw])
            generator = torch.Generator("cuda").manual_seed(seed + x * 7919 + y)
            out = pipe(prompt=prompt, negative_prompt=NEGATIVE, image=piece, control_image=piece, strength=args.strength,
                       num_inference_steps=args.steps, guidance_scale=args.guidance,
                       controlnet_conditioning_scale=args.control, generator=generator).images[0]
            wgt = tile_weights(tw, th, x > 0, y > 0, x + tw < cw, y + th < ch)
            total[y:y + th, x:x + tw] += np.asarray(out, np.float32) * wgt
            weight[y:y + th, x:x + tw] += wgt
    painted = (total / np.maximum(weight, 1e-6)).clip(0, 255).astype(np.uint8)
    return np.asarray(Image.fromarray(painted).resize((w, h), Image.LANCZOS), np.float32)


def broad(picture, radius):
    return np.asarray(Image.fromarray(picture.clip(0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(radius)), np.float32)


def redraw(pipe, dump_file, pack_file, args, work):
    clean = clean_enlargement(dump_file, pack_file, args.upscayl, work)
    rgba = np.asarray(clean, np.float32)
    rgb, alpha = rgba[:, :, :3], rgba[:, :, 3:]
    original = Image.open(dump_file)
    terrain = original.width - 2 * MARGIN >= 400
    seed = int(os.path.splitext(os.path.basename(dump_file))[0][:8], 16)
    detailed = repaint(pipe, rgb.astype(np.uint8), args, TERRAIN_PROMPT if terrain else PROMPT, seed)
    # Fine detail from the repainted picture on top of the broad colours and shading of the clean one, so the
    # picture keeps the game's palette and only gains detail.
    radius = SCALE * args.colour_blur
    merged = detailed - broad(detailed, radius) + broad(rgb, radius)
    result = np.concatenate([merged.clip(0, 255), alpha], axis=2).astype(np.uint8)
    # The palette goes first and the picture last, both through temporary files: the game reads a pack picture
    # as soon as the PNG changes, and takes its palette at that moment.
    palette = os.path.splitext(dump_file)[0] + ".pal"
    if os.path.exists(palette):
        with open(palette, "rb") as f:
            data = f.read()
        target = os.path.splitext(pack_file)[0] + ".pal"
        with open(target + ".tmp", "wb") as f:
            f.write(data)
        replace(target + ".tmp", target)
    temporary = pack_file + ".tmp.png"
    Image.fromarray(result, "RGBA").save(temporary)
    replace(temporary, pack_file)
    open(marker(pack_file), "w").close()


def replace(source, target):
    """Moves source over target. A running game may be reading the target, which Windows refuses for a while, so
    the move is tried again for a few seconds before it gives up."""
    for attempt in range(20):
        try:
            os.replace(source, target)
            return
        except PermissionError:
            if attempt == 19:
                os.remove(source)
                raise
            time.sleep(0.25)


def pending(folder, rebuild):
    dump = os.path.join(folder, "dump")
    pack = os.path.join(folder, "pack")
    names = sorted(n for n in os.listdir(dump) if n.endswith(".png")) if os.path.isdir(dump) else []
    for name in names:
        pack_file = os.path.join(pack, name)
        if rebuild or not os.path.exists(marker(pack_file)):
            yield os.path.join(dump, name), pack_file


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--folder", default=default_folder(), help="the textures folder with dump and pack")
    parser.add_argument("--upscayl", default=os.path.join(os.environ.get("ProgramFiles", ""), "Upscayl", "resources"))
    parser.add_argument("--strength", type=float, default=0.45, help="how much SDXL may change (0 to 1)")
    parser.add_argument("--steps", type=int, default=30)
    parser.add_argument("--guidance", type=float, default=5.0)
    parser.add_argument("--control", type=float, default=0.8, help="how closely SDXL follows the clean picture")
    parser.add_argument("--colour-blur", type=float, default=2.0, help="radius in game pixels of the kept colours")
    parser.add_argument("--rebuild", action="store_true", help="repaint pictures that were repainted before")
    parser.add_argument("--watch", action="store_true", help="keep running and repaint new dumps as they appear")
    parser.add_argument("--only", nargs="*", default=[], help="repaint only these pictures (key prefixes)")
    args = parser.parse_args()

    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    os.makedirs(os.path.join(args.folder, "pack", "redrawn"), exist_ok=True)
    pipe = load_pipeline()
    done = failed = 0
    attempts = {}  # failures per picture in this run; a failure may be passing, such as a full graphics memory
    with tempfile.TemporaryDirectory() as work:
        while True:
            todo = [t for t in pending(args.folder, args.rebuild)
                    if (not args.only or any(os.path.basename(t[0]).startswith(o) for o in args.only))
                    and attempts.get(t[0], 0) < 3]
            for i, (dump_file, pack_file) in enumerate(todo):
                started = time.time()
                try:
                    redraw(pipe, dump_file, pack_file, args, work)
                    done += 1
                    print(f"[{i + 1}/{len(todo)}] {os.path.basename(dump_file)} in {time.time() - started:.1f} s", flush=True)
                except Exception as error:  # one bad picture must not stop the others
                    failed += 1
                    attempts[dump_file] = attempts.get(dump_file, 0) + 1
                    print(f"[{i + 1}/{len(todo)}] {os.path.basename(dump_file)} failed: {error}", flush=True)
                    # No marker is written, so the picture is tried again: up to three times in this run, and
                    # in every later run.
            args.rebuild = False
            if not args.watch:
                break
            time.sleep(5)
    print(f"Redrew {done} pictures, {failed} failed.")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
