import os
import struct
import numpy as np
from PIL import Image

def load_raw_dump(path):
    if not os.path.exists(path):
        print(f"[ERROR] File not found: {path}")
        return None, None
    with open(path, "rb") as f:
        header_data = f.read(16)
        width, height, fmt, rowPitch = struct.unpack("<IIII", header_data)
        print(f"[INFO] Loaded {os.path.basename(path)}: {width}x{height}, format={fmt}, rowPitch={rowPitch}")
        raw_pixels = f.read(width * height * 4)
        arr = np.frombuffer(raw_pixels, dtype=np.uint8).reshape((height, width, 4))
        return arr, (width, height)

def main():
    base_dir = r"C:\code\hlx-dlss5"
    input_path = os.path.join(base_dir, "dump_input.raw")
    output_path = os.path.join(base_dir, "dump_output.raw")

    print("[ANALYSIS] Reading dumped frames from HLX-DLSS5...")
    img_in, dim_in = load_raw_dump(input_path)
    img_out, dim_out = load_raw_dump(output_path)

    if img_in is None or img_out is None:
        print("[WARN] Dumps not yet available on disk. Launch the game with DumpFrames=1 to capture.")
        return

    # Basic stats
    print(f"Input stats  : Min={img_in.min()}, Max={img_in.max()}, Mean={img_in.mean():.2f}, Std={img_in.std():.2f}")
    print(f"Output stats : Min={img_out.min()}, Max={img_out.max()}, Mean={img_out.mean():.2f}, Std={img_out.std():.2f}")

    # Color difference
    diff = np.abs(img_out[:, :, :3].astype(np.int16) - img_in[:, :, :3].astype(np.int16))
    diff_mean = diff.mean()
    diff_max = diff.max()
    altered_pixels = (diff > 0).any(axis=2).sum()
    total_pixels = dim_in[0] * dim_in[1]
    pct_altered = (altered_pixels / total_pixels) * 100.0

    print("--------------------------------------------------")
    print(f"DIFFERENCE ANALYSIS:")
    print(f"  Mean Absolute Difference (MAD) : {diff_mean:.4f} / 255")
    print(f"  Max Absolute Difference        : {diff_max} / 255")
    print(f"  Altered Pixels                 : {altered_pixels:,} / {total_pixels:,} ({pct_altered:.2f}%)")
    print("--------------------------------------------------")

    # Downscale and save preview PNGs for quick visual review
    preview_scale = 0.25
    pw = int(dim_in[0] * preview_scale)
    ph = int(dim_in[1] * preview_scale)

    im_in_pil = Image.fromarray(img_in[:, :, :3]).resize((pw, ph), Image.Resampling.BILINEAR)
    im_in_pil.save(os.path.join(base_dir, "preview_input.png"))

    im_out_pil = Image.fromarray(img_out[:, :, :3]).resize((pw, ph), Image.Resampling.BILINEAR)
    im_out_pil.save(os.path.join(base_dir, "preview_output.png"))

    # Amplified difference map (x5 for visual inspection)
    diff_amp = np.clip(diff * 5, 0, 255).astype(np.uint8)
    im_diff_pil = Image.fromarray(diff_amp).resize((pw, ph), Image.Resampling.BILINEAR)
    im_diff_pil.save(os.path.join(base_dir, "preview_diff_amplified.png"))

    print(f"[SUCCESS] Saved preview PNGs to {base_dir} (preview_input.png, preview_output.png, preview_diff_amplified.png)")

if __name__ == "__main__":
    main()
