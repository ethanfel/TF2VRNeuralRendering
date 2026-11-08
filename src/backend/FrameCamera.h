#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace tf2vrnr {
// Internal matrices multiply column vectors. Transpose only at the NGX ABI.
using Matrix4 = std::array<float, 16>;
inline Matrix4 Identity4() {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}
inline Matrix4 Multiply4(const Matrix4 &a, const Matrix4 &b) {
  Matrix4 r{};
  for (unsigned y = 0; y < 4; ++y)
    for (unsigned x = 0; x < 4; ++x)
      for (unsigned k = 0; k < 4; ++k)
        r[y * 4 + x] += a[y * 4 + k] * b[k * 4 + x];
  return r;
}
inline Matrix4 Transpose4(const Matrix4 &a) {
  Matrix4 r{};
  for (unsigned y = 0; y < 4; ++y)
    for (unsigned x = 0; x < 4; ++x)
      r[y * 4 + x] = a[x * 4 + y];
  return r;
}
inline bool Inverse4(const Matrix4 &a, Matrix4 &result) {
  double m[4][8]{};
  for (unsigned y = 0; y < 4; ++y)
    for (unsigned x = 0; x < 4; ++x) {
      m[y][x] = a[y * 4 + x];
      m[y][x + 4] = x == y;
    }
  for (unsigned x = 0; x < 4; ++x) {
    unsigned pivot = x;
    for (unsigned y = x + 1; y < 4; ++y)
      if (std::abs(m[y][x]) > std::abs(m[pivot][x]))
        pivot = y;
    if (!std::isfinite(m[pivot][x]) || std::abs(m[pivot][x]) < 1e-12)
      return false;
    for (unsigned k = 0; k < 8; ++k)
      std::swap(m[x][k], m[pivot][k]);
    const double scale = m[x][x];
    for (auto &v : m[x])
      v /= scale;
    for (unsigned y = 0; y < 4; ++y)
      if (y != x) {
        const double s = m[y][x];
        for (unsigned k = 0; k < 8; ++k)
          m[y][k] -= s * m[x][k];
      }
  }
  for (unsigned y = 0; y < 4; ++y)
    for (unsigned x = 0; x < 4; ++x)
      result[y * 4 + x] = static_cast<float>(m[y][x + 4]);
  return true;
}
struct FrameCamera {
  Matrix4 worldToClip{}, viewToClip{};
  std::array<float, 3> position{}, right{}, up{}, forward{};
  float nearPlane = 0, farPlane = 0, fov = 0, aspect = 0;
  bool valid = false;
  const char* rejection="uninitialized";
};
struct FrameCameraProjection {
  float left=0,right=0,up=0,down=0; // Tangents of the original OpenXR FOV.
  bool valid=false;
};
// Titanfall CBufCommonPerCamera is 576 bytes. Require the full-size perspective
// world view: the UI and compressed-depth weapon cameras must never qualify.
inline FrameCamera DecodeFrameCamera(const void *bytes, unsigned width,
                                     unsigned height, float jitterX = 0,
                                     float jitterY = 0,const FrameCameraProjection* expected=nullptr) {
  FrameCamera r;
  float v[144];
  std::memcpy(v, bytes, sizeof(v));
  auto reject=[&](const char* reason){r.rejection=reason;return r;};
  const bool xrProjection=expected&&expected->valid;
  if(!width||!height)return reject("zero_size");
  // The VR renderer can retain desktop sizing/near-plane metadata in b2.
  // With a fresh XR projection, validate against the actual eye frustum instead.
  // The hook also requires an eye-sized HDR target and a full-depth viewport.
  if(!xrProjection) {
    for(unsigned index:{0u,114u,115u,135u,136u,137u})if(!std::isfinite(v[index]))return reject("metadata_nonfinite");
    if(v[135]<.999f)return reject("metadata_weapon_depth");
    if(std::abs(v[114]*v[136]-width)>2||std::abs(v[115]*v[137]-height)>2)return reject("metadata_size");
  }
  // Unused shader constants may contain infinity (e.g. infinite far planes).
  // Only values consumed by the camera decoder must be finite.
  for(unsigned index=1;index<20;++index)if(!std::isfinite(v[index]))return reject("camera_nonfinite");
  Matrix4 c;
  std::copy(v + 4, v + 20, c.begin());
  auto dot = [&](unsigned a, unsigned b) {
    float s = 0;
    for (unsigned k = 0; k < 3; ++k)
      s += c[4 * a + k] * c[4 * b + k];
    return s;
  };
  float l = dot(3, 3);
  if (l < .99f || l > 1.01f || std::abs(c[15]) > 1e-4f)
    return reject("not_perspective");
  const float k = dot(2, 3) / l;
  if (k < .5f || k > 1.5f)
    return reject("depth_scale");
  r.nearPlane = -c[11] / k;
  if(r.nearPlane<=.0001f||r.nearPlane>=64)return reject("near_plane");
  if(!xrProjection&&(r.nearPlane<=.5f||std::abs(v[0]/v[135]-r.nearPlane)>.02f*r.nearPlane+.001f))return reject("metadata_near");
  // DLAA returns an unjittered eye image; remove its image displacement.
  for (unsigned x = 0; x < 4; ++x) {
    c[x] -= 2 * jitterX / width * c[12 + x];
    c[4 + x] += 2 * jitterY / height * c[12 + x];
  }
  const float ox = dot(0, 3) / l, oy = dot(1, 3) / l;
  const float sx = std::sqrt(std::max(0.f, dot(0, 0) / l - ox * ox));
  const float sy = std::sqrt(std::max(0.f, dot(1, 1) / l - oy * oy));
  if (sx < .01f || sy < .01f)
    return reject("degenerate_projection");
  if(xrProjection) {
    const auto& p=*expected;
    const float spanX=p.right-p.left,spanY=p.up-p.down;
    if(!std::isfinite(spanX)||!std::isfinite(spanY)||spanX<=.01f||spanY<=.01f)return reject("invalid_xr_projection");
    const float expectedSx=2/spanX,expectedSy=2/spanY;
    const float expectedOx=-(p.right+p.left)/spanX,expectedOy=-(p.up+p.down)/spanY;
    if(std::abs(sx-expectedSx)>.01f*std::max(1.f,expectedSx)||std::abs(sy-expectedSy)>.01f*std::max(1.f,expectedSy)||
       std::abs(ox-expectedOx)>.01f||std::abs(oy-expectedOy)>.01f)return reject("xr_projection_mismatch");
  }
  for (unsigned j = 0; j < 3; ++j) {
    r.position[j] = v[1 + j];
    r.forward[j] = c[12 + j] / std::sqrt(l);
    r.right[j] = (c[j] - ox * c[12 + j]) / sx;
    r.up[j] = (c[4 + j] - oy * c[12 + j]) / sy;
  }
  r.farPlane = k > 1.000001f ? r.nearPlane * k / (k - 1) : 1e7f;
  if (r.farPlane <= r.nearPlane || r.farPlane > 1e7f)
    return reject("far_plane");
  r.fov = std::atan((1 - oy) / sy) + std::atan((1 + oy) / sy);
  r.aspect = sy / sx;
  if (r.fov <= .05f || r.fov >= 3.1f)
    return reject("field_of_view");
  r.viewToClip = {sx, 0, -ox, 0, 0, sy, -oy, 0, 0, 0, -k, -r.nearPlane * k,
                  0,  0, -1,  0};
  r.worldToClip = c;
  for (unsigned y = 0; y < 4; ++y)
    for (unsigned x = 0; x < 3; ++x)
      r.worldToClip[y * 4 + 3] -= c[y * 4 + x] * r.position[x];
  Matrix4 inverse;
  r.valid = Inverse4(r.worldToClip, inverse);
  r.rejection=r.valid?"accepted":"singular_matrix";
  return r;
}
inline bool CameraCut(const FrameCamera &a, const FrameCamera &b) {
  if (!a.valid || !b.valid)
    return true;
  float distance = 0, dot = 0;
  for (unsigned k = 0; k < 3; ++k) {
    float d = a.position[k] - b.position[k];
    distance += d * d;
    dot += a.forward[k] * b.forward[k];
  }
  return distance > 64 * 64 || dot < .866f || std::abs(a.fov - b.fov) > .0873f;
}
} // namespace tf2vrnr
