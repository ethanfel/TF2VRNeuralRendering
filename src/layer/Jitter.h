#pragma once
#include <cmath>
#include <cstdint>
#include <openxr.h>
namespace tf2vrnr {
struct PixelJitter { float x=0, y=0; };
inline float Halton(unsigned index,unsigned base) {
    float value=0,scale=1;
    while(index) {scale/=base;value+=scale*(index%base);index/=base;}
    return value;
}
inline PixelJitter DlaaJitter(uint64_t frame) {
    const unsigned phase=static_cast<unsigned>(frame%16)+1;
    return {Halton(phase,2)-0.5f,Halton(phase,3)-0.5f};
}
// Jitter denotes the subpixel sample location, x right/y down. Translate
// the frustum centre without changing its tangent-space width/height.
inline XrFovf JitterFov(XrFovf f,unsigned width,unsigned height,PixelJitter j) {
    const float left=std::tan(f.angleLeft),right=std::tan(f.angleRight);
    const float up=std::tan(f.angleUp),down=std::tan(f.angleDown);
    const float dx=(right-left)*j.x/width,dy=-(up-down)*j.y/height;
    return {std::atan(left+dx),std::atan(right+dx),std::atan(up+dy),std::atan(down+dy)};
}
}
