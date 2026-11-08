#pragma once
#include "Graphics.h"
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <array>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <openxr.h>
#include <openxr_platform.h>
#include <vector>
namespace tf2vrnr {
struct StereoFrameTextures {
  std::shared_ptr<void> owner;
  ID3D11Texture2D *real[2]{};
  ID3D11Texture2D *generated[2]{};
  int64_t format[2]{};
  bool interpolated = false;
  explicit operator bool() const {
    return owner && real[0] && real[1] && generated[0] && generated[1];
  }
};
bool CopyPresentationImage(ID3D11Device *, ID3D11Texture2D *destination,
                           ID3D11Texture2D *source);
struct FrameDeliveryDispatch {
  PFN_xrWaitFrame wait = nullptr;
  PFN_xrBeginFrame begin = nullptr;
  PFN_xrEndFrame end = nullptr;
  PFN_xrCreateSwapchain create = nullptr;
  PFN_xrDestroySwapchain destroy = nullptr;
  PFN_xrEnumerateSwapchainImages enumerate = nullptr;
  PFN_xrAcquireSwapchainImage acquire = nullptr;
  PFN_xrWaitSwapchainImage waitImage = nullptr;
  PFN_xrReleaseSwapchainImage release = nullptr;
};
// Synchronous 2x pacing: one app frame spans two runtime frames. App
// xrWaitFrame calls are serialized only while FG is requested; internal calls
// go directly downstream. No compositor-owned image is read or written after
// release.
class FrameDelivery {
  XrSession session_;
  ID3D11Device *device_;
  FrameDeliveryDispatch api_;
  std::mutex mutex_;
  std::condition_variable finished_;
  bool busy_ = false, paired_ = false, ready_ = false, fault_ = false;
  unsigned pairs_=0,generatedPairs_=0;
  XrTime time_ = 0, previousTime_ = 0;
  XrDuration period_ = 0;
  XrSpace previousSpace_ = XR_NULL_HANDLE;
  std::array<XrCompositionLayerProjectionView, 2> previousViews_{};
  struct Chain {
    XrSwapchain handle = XR_NULL_HANDLE;
    unsigned width = 0, height = 0;
    int64_t format = 0;
    std::vector<ID3D11Texture2D *> images;
  };
  std::array<Chain, 2> chains_;
  bool Ensure(const XrCompositionLayerProjection &,
              const StereoFrameTextures &);
  bool Fill(const StereoFrameTextures &, bool generated);
  XrResult Emit(const XrFrameEndInfo &, const XrCompositionLayerProjection &,
                bool generated, XrTime time);
  void Finish();

public:
  FrameDelivery(XrSession, ID3D11Device *, const FrameDeliveryDispatch &);
  ~FrameDelivery();
  XrResult Wait(const XrFrameWaitInfo *, XrFrameState *, bool enabled);
  XrResult End(const XrFrameEndInfo *, const StereoFrameTextures &);
  // Called after the outer layer has armed the next frame's camera/guides.
  void Complete() { Finish(); }
  unsigned GeneratedPairs()const{return generatedPairs_;}
};
} // namespace tf2vrnr
