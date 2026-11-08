#!/usr/bin/env python3
"""Ask the running graphics layer to save its configured stereo burst; no runtime changes."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--package", type=Path, default=Path(__file__).resolve().parent,
                    help="Installed Local-TF2VRNeuralRendering-0.1.0 directory")
args = parser.parse_args()
layer = args.package.resolve() / "plugins" / "layer"
if not (layer / "TF2VRNeuralRenderingLayer.dll").is_file():
    parser.error("Choose the installed package with --package; its layer DLL was not found")
(layer / "capture.request").touch()
print("Capture requested. With the game running in VR, the configured 1–2 stereo frames will be saved under:")
print(layer / "captures")
print("Capture metadata records which effects were applied to both eyes.")
