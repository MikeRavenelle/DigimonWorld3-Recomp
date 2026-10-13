#!/usr/bin/env python3
"""Converts the official Real-ESRGAN "general-x4v3" weights to the ncnn files realesrgan-ncnn-vulkan loads.

  1. downloads realesr-general-x4v3.pth (xinntao/Real-ESRGAN release v0.2.5.0, BSD 3-Clause; see LICENSE-Real-ESRGAN.txt)
  2. builds the network (SRVGGNetCompact, 32 convolutions, x4) in PyTorch and traces it to TorchScript
  3. converts the TorchScript file to ncnn with pnnx
  4. renames the blobs to "data" and "output", the names the upscaler program looks up
  5. writes RealESRGAN_General_x4_v3.param/.bin and prints their SHA-256 (the constants in app/wizard/assets.cpp)

Needs: python3, torch, and pnnx (`pip install torch pnnx`, or a pnnx binary on PATH / --pnnx).
Redistribute the .param and .bin together with LICENSE-Real-ESRGAN.txt (BSD 3-Clause requires the notice).
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

WEIGHTS_URL = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-x4v3.pth"
MODEL_NAME = "RealESRGAN_General_x4_v3"
TRACE_SIZE = 64


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, destination: Path) -> None:
    if destination.exists():
        return
    print(f"downloading {url}")
    with urllib.request.urlopen(url) as response, open(destination, "wb") as out:
        shutil.copyfileobj(response, out)


def build_network(torch):
    nn = torch.nn
    functional = torch.nn.functional

    class SRVGGNetCompact(nn.Module):
        """The compact VGG-style super-resolution network of Real-ESRGAN (BSD 3-Clause, Xintao Wang)."""

        def __init__(self, num_in_ch=3, num_out_ch=3, num_feat=64, num_conv=32, upscale=4):
            super().__init__()
            self.upscale = upscale
            self.body = nn.ModuleList()
            self.body.append(nn.Conv2d(num_in_ch, num_feat, 3, 1, 1))
            self.body.append(nn.PReLU(num_parameters=num_feat))
            for _ in range(num_conv):
                self.body.append(nn.Conv2d(num_feat, num_feat, 3, 1, 1))
                self.body.append(nn.PReLU(num_parameters=num_feat))
            self.body.append(nn.Conv2d(num_feat, num_out_ch * upscale * upscale, 3, 1, 1))
            self.upsampler = nn.PixelShuffle(upscale)

        def forward(self, x):
            out = x
            for layer in self.body:
                out = layer(out)
            out = self.upsampler(out)
            return out + functional.interpolate(x, scale_factor=self.upscale, mode="nearest")

    return SRVGGNetCompact()


def run_pnnx(pnnx_path, traced: Path, work: Path) -> None:
    executable = pnnx_path or shutil.which("pnnx")
    if not executable:
        sys.exit("pnnx not found: `pip install pnnx` (into the active environment), or pass --pnnx PATH")
    subprocess.run([executable, str(traced), f"inputshape=[1,3,{TRACE_SIZE},{TRACE_SIZE}]"], cwd=work, check=True)


def rename_blobs(param: Path) -> None:
    """pnnx names the blobs in0/out0; realesrgan-ncnn-vulkan asks for "data" and "output"."""
    lines = param.read_text().splitlines()
    renamed = [" ".join({"in0": "data", "out0": "output"}.get(token, token) for token in line.split(" ")) for line in lines]
    param.write_text("\n".join(renamed) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=Path("tools/models/out"), help="folder for the finished files")
    parser.add_argument("--work", type=Path, default=Path("tools/models/work"), help="folder for downloads and intermediate files")
    parser.add_argument("--pnnx", help="pnnx executable (default: the one on PATH)")
    args = parser.parse_args()

    try:
        import torch
    except ImportError:
        sys.exit("torch is not installed: pip install torch (CPU build is enough), and pnnx")

    args.work.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)
    weights = args.work / "realesr-general-x4v3.pth"
    download(WEIGHTS_URL, weights)

    network = build_network(torch)
    state = torch.load(weights, map_location="cpu")
    network.load_state_dict(state.get("params_ema", state.get("params", state)), strict=True)
    network.eval()

    traced_path = args.work / f"{MODEL_NAME}.pt"
    example = torch.rand(1, 3, TRACE_SIZE, TRACE_SIZE)
    torch.jit.trace(network, example).save(str(traced_path))

    run_pnnx(args.pnnx, traced_path, args.work)
    param_src = args.work / f"{MODEL_NAME}.ncnn.param"
    bin_src = args.work / f"{MODEL_NAME}.ncnn.bin"
    if not param_src.exists() or not bin_src.exists():
        sys.exit(f"pnnx did not produce {param_src.name} and {bin_src.name} in {args.work}")
    rename_blobs(param_src)
    param, binary = args.out / f"{MODEL_NAME}.param", args.out / f"{MODEL_NAME}.bin"
    shutil.copy(param_src, param)
    shutil.copy(bin_src, binary)
    shutil.copy(Path(__file__).with_name("LICENSE-Real-ESRGAN.txt"), args.out / "LICENSE-Real-ESRGAN.txt")

    print("\nupload these files, with LICENSE-Real-ESRGAN.txt, to a release of this project:")
    for path in (param, binary):
        print(f"  {path.name}  {path.stat().st_size} bytes  sha256 {sha256(path)}")
    print(f"\nsource weights sha256 {sha256(weights)}")
    print("then set kModelParamSha256 and kModelBinSha256 (and kModelReleaseUrl) in app/wizard/assets.cpp")
    return 0


if __name__ == "__main__":
    sys.exit(main())
