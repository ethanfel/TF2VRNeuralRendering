#pragma once
#include "State.h"
#include <d3d11.h>
#include <memory>
#include <string>
namespace tf2vrnr {
struct CapturedImage {
    virtual ~CapturedImage() = default;
    // Called only on the graphics thread. Success means this object and all
    // retained candidates contain CPU bytes only, with no live D3D references.
    virtual bool PrepareForAsyncWrite() = 0;
    virtual bool WriteView(const ViewLayout& view, const std::wstring& filename) = 0;
    virtual const std::string& BindingsJson() const { static const std::string empty; return empty; }
    virtual void WriteCandidates(const ViewLayout&, const std::wstring&) {}
};
// Must be called BEFORE forwarding xrReleaseSwapchainImage. The returned
// object owns a staging copy: it never reads the runtime texture after release.
std::shared_ptr<CapturedImage> CaptureImage(ID3D11Device* device, ID3D11Texture2D* source, int64_t format,
    bool inspectBindings, bool captureCandidates = false);
}
