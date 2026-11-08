# Runtime requirements

Start with a working CircuitLord Titanfall 2 VR installation. This addon does not install the base mod, Proton, a graphics driver, or an OpenXR runtime.

The tested setup is an RTX 5090 with Linux NVIDIA driver 615.78.08, Proton-TF2VR, DXVK, vkd3d-proton, NVIDIA NGX support, WiVRn and Quest 3. Compatibility with other GPUs, drivers and runtimes has not been established.

## NVIDIA files

Create `plugins/layer/runtime/` inside the installed addon and place the required **Windows x64** runtime DLLs there:

- `nvngx_dlssnr.dll`: Neural Rendering. The tested file is version 310.9.16.0 from [NVIDIA's RTX Remix NR dependency archive](https://d1oqksizpb0e3d.cloudfront.net/rtx-remix-ngx_sdk_dlnr@2.7z), referenced in the [RTX Remix dependency manifest](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/packman-external.xml). Extract this DLL from the archive. This runtime declares a minimum driver version of 615.00.
- `nvngx_dlss.dll`: native-resolution DLAA. Available from [NVIDIA's Windows x64 release files](https://github.com/NVIDIA/DLSS/tree/main/lib/Windows_x86_64/rel).
- `nvngx_dlssg.dll`: optional experimental frame generation, available from the same NVIDIA directory.

These DLLs serve different features and cannot substitute for each other. Downloads and use remain subject to NVIDIA's terms. They are not included in this repository or its release ZIP.

Known tested SHA-256 values:

```text
2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f  nvngx_dlssnr.dll
d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e  nvngx_dlss.dll
8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7  nvngx_dlssg.dll
```

Newer downloads may have different hashes and are not automatically covered by those tests.

## Proton

Proton needs its NVIDIA NGX bridge and matching driver-provided Wine NGX components. The tested environment enabled NVAPI and exposed the NVIDIA GPU (`PROTON_ENABLE_NVAPI=1`, `PROTON_HIDE_NVIDIA_GPU=0`). Driver-core/runtime mismatches can prevent initialization even when all three DLLs are present.

Launch through your existing working VR setup. The addon registers its OpenXR layer in that game's prefix and keeps your selected runtime. If it registers on the first launch, close and restart the game once.

Check `plugins/layer/TF2VRNeuralRendering.log` for initialization errors. Successful eye processing includes `neural_evaluate` or `dlaa_evaluate` with `output_applied=1`. The menu shows requested settings; it cannot certify successful rendering.

## Disable or remove

Use **All effects off** in the menu to stop the three processing stages. To bypass the entire layer, set `DISABLE_TF2VR_NEURAL_RENDERING=1` when launching the game.

To uninstall, close the game, run `TF2VRNeuralRenderingSetup.exe unregister` through the same Wine/Proton prefix, then remove the addon package folder. On native Windows, run the helper directly; that platform is not yet tested for rendering.
