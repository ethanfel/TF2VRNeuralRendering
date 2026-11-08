# Third-party notices

The Northstar bootstrap interfaces and registration implementation are adapted
from TF2VRSeatedControls, Copyright (c) 2026 Ethanfel and contributors, MIT.
Its bootstrap was adapted from Nighteyes' TF2VRBetterBindings:
https://github.com/Fdrph/TF2VRBetterBindings
The applicable notice is preserved in licenses/BetterBindings-LICENSE.txt.

OpenXR headers are build dependencies from KhronosGroup/OpenXR-SDK and are not
redistributed in this package. API names and structures follow the OpenXR
specification. MinHook v1.3.4 (Tsuda Kageyu and contributors), commit
c3fcafdc10146beb5919319d0683e44e3c30d537, is statically linked for requested
depth-clear and render-target observers. Its BSD notices (including the HDE component) are
preserved in licenses/MinHook-LICENSE.txt and the vendored source headers.
Source: https://github.com/TsudaKageyu/minhook

The neural backend is original addon code. NVIDIA common and Neural Rendering
SDK headers are external build dependencies subject to their own license.
DLAA follows NVIDIA's DLSS SDK programming guide and public parameter definitions:
https://github.com/NVIDIA/DLSS
The Halton sampler, OpenXR adaptation and GPU conversion code are original.
NVIDIA SDK headers and runtime DLLs are external dependencies. ReShade, model
weights, game assets and CircuitLord binaries are not bundled.

Builds use NVIDIA DLSS/NGX SDK headers. This software contains source code
provided by NVIDIA Corporation. NVIDIA components remain governed by their
own terms; see licenses/NVIDIA-RTX-SDKs-LICENSE.txt. The MIT license applies
to the original addon code, not those external SDK components.

The menu uses Northstar's public Squirrel Mod Settings and VGUI interfaces.
ConVar layout research was checked against NorthstarLauncher tier1 headers and
the installed engine's integer setter:
https://github.com/R2Northstar/NorthstarLauncher/tree/main/primedev/tier1
The menu script, resource, persistence bridge and tests are original addon code;
they do not include or replace CircuitLord's menu scripts or assets.

Frame generation follows NVIDIA's standalone DLSS-FG programming guide and
parameter definitions, with external SDK headers and a separately installed
runtime: https://github.com/NVIDIA/DLSS
The OpenXR scheduling, image ownership and stereo interpolation code are original.

Rendering research references include the MIT-licensed flat-screen bridge
(https://github.com/sorecandy/tf2-ai-enhancements) for the engine's render-target
layout, motion encoding and documented CBufCommonPerCamera offsets/matrix
interpretation, and FoveatedDLSS5
(https://github.com/maximedgoulet/FoveatedDLSS5) for display-referred neural
inputs. Their renderer code is not included in this package.

Titanfall is a trademark of its respective owners. This add-on is not affiliated
with CircuitLord, Respawn, Electronic Arts, NVIDIA or the WiVRn project.
