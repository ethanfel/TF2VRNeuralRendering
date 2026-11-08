#include "FrameDelivery.h"
#include "Support.h"
#include <cmath>
#include <cstring>
namespace tf2vrnr {
namespace {
const XrCompositionLayerProjection *Stereo(const XrFrameEndInfo *info) {
  if (!info || info->next || info->layerCount != 1 || !info->layers ||
      !info->layers[0] ||
      info->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
    return nullptr;
  auto *header = info->layers[0];
  if (header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION || header->next)
    return nullptr;
  auto *p = reinterpret_cast<const XrCompositionLayerProjection *>(header);
  if (p->viewCount != 2 || !p->views)
    return nullptr;
  for (unsigned e = 0; e < 2; ++e) {
    const auto &v = p->views[e];
    if (v.next || v.subImage.imageArrayIndex || v.subImage.imageRect.offset.x ||
        v.subImage.imageRect.offset.y ||
        v.subImage.imageRect.extent.width <= 0 ||
        v.subImage.imageRect.extent.height <= 0)
      return nullptr;
  }
  return p;
}
XrPosef Midpoint(const XrPosef &a, const XrPosef &b) {
  XrPosef r{};
  r.position = {(a.position.x + b.position.x) * .5f,
                (a.position.y + b.position.y) * .5f,
                (a.position.z + b.position.z) * .5f};
  float dot =
      a.orientation.x * b.orientation.x + a.orientation.y * b.orientation.y +
      a.orientation.z * b.orientation.z + a.orientation.w * b.orientation.w;
  float sign = dot < 0 ? -1.f : 1.f;
  r.orientation = {a.orientation.x + sign * b.orientation.x,
                   a.orientation.y + sign * b.orientation.y,
                   a.orientation.z + sign * b.orientation.z,
                   a.orientation.w + sign * b.orientation.w};
  auto &q = r.orientation;
  float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (n > 1e-6f) {
    q.x /= n;
    q.y /= n;
    q.z /= n;
    q.w /= n;
  } else
    q = b.orientation;
  return r;
}
} // namespace
FrameDelivery::FrameDelivery(XrSession s, ID3D11Device *d,
                             const FrameDeliveryDispatch &a)
    : session_(s), device_(d), api_(a) {}
FrameDelivery::~FrameDelivery() {
  for (auto &c : chains_)
    if (c.handle)
      api_.destroy(c.handle);
}
void FrameDelivery::Finish() {
  std::lock_guard lock(mutex_);
  busy_ = false;
  finished_.notify_all();
}
XrResult FrameDelivery::Wait(const XrFrameWaitInfo *info, XrFrameState *state,
                             bool enabled) {
  {
    std::unique_lock lock(mutex_);
    finished_.wait(lock, [&] { return !busy_; });
    if (enabled && !fault_)
      busy_ = true;
    else {
      ready_ = false;
      paired_ = false;
    }
  }
  const auto r = api_.wait(session_, info, state);
  if (XR_FAILED(r) || !state) {
    Finish();
    return r;
  }
  std::lock_guard lock(mutex_);
  if (!busy_)
    return r;
  time_ = state->predictedDisplayTime;
  period_ = state->predictedDisplayPeriod;
  paired_ = ready_ && state->shouldRender && period_ >= 1000000 &&
            period_ <= 100000000;
  if (paired_) {
    state->predictedDisplayTime += period_;
    state->predictedDisplayPeriod *= 2;
  }
  return r;
}
bool FrameDelivery::Ensure(const XrCompositionLayerProjection &p,
                           const StereoFrameTextures &t) {
  for (unsigned e = 0; e < 2; ++e) {
    auto &c = chains_[e];
    const auto size = p.views[e].subImage.imageRect.extent;
    if (t.format[e] != 28 && t.format[e] != 29)
      return false;
    if (c.handle &&
        (c.width != unsigned(size.width) || c.height != unsigned(size.height) ||
         c.format != t.format[e])) {
      api_.destroy(c.handle);
      c = {};
      ready_ = false;
    }
    if (c.handle)
      continue;
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                      XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                      XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = t.format[e];
    info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
    info.width = size.width;
    info.height = size.height;
    if (XR_FAILED(api_.create(session_, &info, &c.handle)))
      return false;
    uint32_t count = 0;
    if (XR_FAILED(api_.enumerate(c.handle, 0, &count, nullptr)) || !count ||
        count > 32)
      return false;
    std::vector<XrSwapchainImageD3D11KHR> images(
        count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_FAILED(api_.enumerate(
            c.handle, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()))))
      return false;
    for (const auto &image : images) {
      if (!image.texture)
        return false;
      c.images.push_back(image.texture);
    }
    c.width = info.width;
    c.height = info.height;
    c.format = info.format;
    Log("fg_presentation_swapchain eye=%u size=%ux%u images=%u", e, c.width,
        c.height, count);
  }
  return true;
}
bool FrameDelivery::Fill(const StereoFrameTextures &t, bool generated) {
  for (unsigned e = 0; e < 2; ++e) {
    auto &c = chains_[e];
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(api_.acquire(c.handle, &a, &index)))
      return false;
    XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    w.timeout = 500000000;
    if (api_.waitImage(c.handle, &w) != XR_SUCCESS || index >= c.images.size())
      return false;
    const bool copied = CopyPresentationImage(
        device_, c.images[index], generated ? t.generated[e] : t.real[e]);
    XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const auto released = api_.release(c.handle, &r);
    if (!copied || XR_FAILED(released))
      return false;
  }
  return true;
}
XrResult FrameDelivery::Emit(const XrFrameEndInfo &info,
                             const XrCompositionLayerProjection &projection,
                             bool generated, XrTime time) {
  std::array<XrCompositionLayerProjectionView, 2> views;
  for (unsigned e = 0; e < 2; ++e) {
    views[e] = projection.views[e];
    views[e].subImage.swapchain = chains_[e].handle;
    if (generated)
      views[e].pose = Midpoint(previousViews_[e].pose, views[e].pose);
  }
  auto p = projection;
  p.views = views.data();
  const auto *pointer =
      reinterpret_cast<const XrCompositionLayerBaseHeader *>(&p);
  auto end = info;
  end.displayTime = time;
  end.layers = &pointer;
  end.layerCount = 1;
  return api_.end(session_, &end);
}
XrResult FrameDelivery::End(const XrFrameEndInfo *info,
                            const StereoFrameTextures &textures) {
  bool active = false, paired = false;
  {
    std::lock_guard lock(mutex_);
    active = busy_;
    paired = paired_;
  }
  if (!active)
    return api_.end(session_, info);
  const auto *p = Stereo(info);
  bool supported = p && textures && !fault_;
  if (supported && !Ensure(*p, textures)) {
    fault_ = true;
    supported = false;
    Log("fg_presentation_disabled swapchain_creation_failed=1");
  }
  bool interpolate =
      paired && supported && textures.interpolated &&
      previousSpace_ == p->space && previousTime_ > 0 &&
      std::abs((info->displayTime - previousTime_) - 2 * period_) < period_ / 4;
  if (interpolate)
    for (unsigned e = 0; e < 2; ++e)
      interpolate = interpolate &&
                    !std::memcmp(&previousViews_[e].fov, &p->views[e].fov,
                                 sizeof(XrFovf)) &&
                    previousViews_[e].subImage.swapchain ==
                        p->views[e].subImage.swapchain;
  XrResult result = XR_SUCCESS;
  if (paired) {
    bool submittedGenerated=false;
    bool filled = supported && Fill(textures, interpolate);
    if (supported && !filled) {
      fault_ = true;
      Log("fg_presentation_disabled image_copy_failed=1");
    }
    if (filled) {
      result = Emit(*info, *p, interpolate, time_);
      submittedGenerated=interpolate&&XR_SUCCEEDED(result);
    } else if (info) {
      auto fallback = *info;
      fallback.displayTime = time_;
      result = api_.end(session_, &fallback);
    } else
      result = api_.end(session_, info);
    if (XR_SUCCEEDED(result)) {
      XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
      XrFrameState state{XR_TYPE_FRAME_STATE};
      result = api_.wait(session_, &wait, &state);
      if (XR_SUCCEEDED(result)) {
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        result = api_.begin(session_, &begin);
        if (XR_SUCCEEDED(result)) {
          bool realFilled=false;
          if(state.shouldRender&&filled) {
            realFilled=Fill(textures,false);
            if(!realFilled){fault_=true;supported=false;Log("fg_presentation_disabled real_image_copy_failed=1");}
          }
          if (realFilled)
            result = Emit(*info, *p, false, state.predictedDisplayTime);
          else {
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = state.predictedDisplayTime;
            end.environmentBlendMode = info ? info->environmentBlendMode
                                            : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            if (state.shouldRender && info) {
              end.layerCount = info->layerCount;
              end.layers = info->layers;
            }
            result = api_.end(session_, &end);
          }
          if (std::abs(state.predictedDisplayTime - (time_ + period_)) >
              period_ / 4)
            supported = false;
        }
      }
    }
    if(submittedGenerated&&XR_SUCCEEDED(result))++generatedPairs_;
    if (++pairs_ <= 4 || pairs_ % 300 == 0)
      Log("fg_present pair=%u generated=%d result=%d period_ns=%lld total_generated_pairs=%u", pairs_,
          submittedGenerated, result, (long long)period_,generatedPairs_);
  } else
    result = api_.end(session_, info);
  if (p) {
    previousSpace_ = p->space;
    previousTime_ = info->displayTime;
    for (unsigned e = 0; e < 2; ++e)
      previousViews_[e] = p->views[e];
  } else {
    previousTime_ = 0;
    previousSpace_ = XR_NULL_HANDLE;
  }
  {
    std::lock_guard lock(mutex_);
    ready_ = supported && XR_SUCCEEDED(result) && !fault_;
    if (XR_FAILED(result))
      fault_ = true;
  }
  return result;
}
} // namespace tf2vrnr
