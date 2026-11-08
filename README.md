# Titanfall 2 VR Neural Rendering

DLSS Neural Rendering and DLAA for [CircuitLord's Titanfall 2 VR mod](https://github.com/CircuitLord/CircuitLordVRModInstaller), with an in-game settings menu.

Tested on Linux with Proton, WiVRn, Quest 3 and an RTX 5090. Other setups are untested.

## Features

- Neural Rendering with models A/B, resolution, strength, tone and detail controls.
- Native-resolution DLAA, independently switchable.
- Experimental VR frame generation. **Leave it off for now:** it made gameplay less smooth in testing.
- Stereo screenshots and an **All effects off** button for comparisons.

DLSS Super Resolution upscaling is not implemented.

## Install

1. Install CircuitLord's VR mod and confirm the game works in VR.
2. Close the game. Download the addon ZIP from [Releases](https://github.com/ethanfel/TF2VRNeuralRendering/releases).
3. Extract `Local-TF2VRNeuralRendering-0.1.0` into `Titanfall2/TF2VR/packages/`.
4. Put the NVIDIA runtime files you need into the addon's `plugins/layer/runtime/` folder:

   | Feature | File | Download |
   | --- | --- | --- |
   | Neural Rendering | `nvngx_dlssnr.dll` | [NVIDIA RTX Remix NR archive](https://d1oqksizpb0e3d.cloudfront.net/rtx-remix-ngx_sdk_dlnr@2.7z) |
   | DLAA | `nvngx_dlss.dll` | [NVIDIA DLSS](https://github.com/NVIDIA/DLSS/tree/main/lib/Windows_x86_64/rel) |
   | Frame generation (optional) | `nvngx_dlssg.dll` | [NVIDIA DLSS](https://github.com/NVIDIA/DLSS/tree/main/lib/Windows_x86_64/rel) |

5. Connect your headset and launch the game normally. Restart once after the first launch to activate the addon.
6. Set the game's anti-aliasing to **None**, then enable the effects you want in the addon menu.

NVIDIA runtimes are separate downloads under NVIDIA's terms. Linux also needs working NVIDIA NGX support in Proton; copying these files alone does not provide it. See [runtime requirements](docs/RUNTIMES.md).

When updating, keep your existing `plugins/layer/TF2VRNeuralRendering.ini` and `runtime/` folder. New installs start with all effects off.

## Controls

Open **Pause → DLSS 5** or **VR Settings → DLSS 5** using the footer button. The menu is also listed in **Mod Settings**.

- **Apply changes** saves your settings and updates the renderer. A short pause is normal.
- **Revert / reload** discards pending edits and loads saved settings.
- **All effects off** disables Neural Rendering, DLAA and frame generation while keeping your tuning values.

The menu controls are included, but their in-headset layout and navigation still need validation in this first experimental release.

Optional terminal controls, run from the installed addon folder:

```sh
python3 control.py status
python3 control.py dlaa on
python3 control.py on
python3 control.py model B
python3 control.py strength 75
python3 control.py fg off
```

Logs are in `plugins/layer/TF2VRNeuralRendering.log`. A saved setting does not guarantee the GPU feature started successfully.

## Development

[Build instructions and tests](docs/BUILD.md). Licensed under [MIT](LICENSE); see [third-party notices](THIRD-PARTY-NOTICES.md).

Independent community addon, unaffiliated with CircuitLord, Respawn, EA or NVIDIA.
