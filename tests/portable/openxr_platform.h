#pragma once
// Same public data layout as XR_KHR_D3D11_enable; only opaque COM pointers are
// needed by routing tests. Production uses the installed Khronos platform header.
struct XrGraphicsBindingD3D11KHR {
    XrStructureType type;
    const void* next;
    ID3D11Device* device;
};
struct XrSwapchainImageD3D11KHR {
    XrStructureType type;
    void* next;
    ID3D11Texture2D* texture;
};
