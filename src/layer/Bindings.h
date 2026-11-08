#pragma once
#include <d3d11.h>
#include <string>
namespace tf2vrnr {
// Read-only immediate-context state at the capture point. These bindings are
// evidence for later investigation, not identified depth/motion/HDR guides.
std::string InspectBindings(ID3D11DeviceContext* context, ID3D11Texture2D* color,
    const char* stage = "before_xrReleaseSwapchainImage");
}
