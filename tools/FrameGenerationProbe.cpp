// Independent real-GPU diagnostic; never launches or injects into the game.
#include "../src/backend/GpuInterop.h"
#include "../src/layer/Support.h"
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cmath>
using namespace tf2vrnr;
using Microsoft::WRL::ComPtr;
extern "C" unsigned int tf2vrnr_parameter_get_int(void*,const char*,int*);
extern "C" void tf2vrnr_parameter_set_int(void*,const char*,int);
extern "C" void tf2vrnr_parameter_set_uint(void*,const char*,unsigned);
extern "C" void tf2vrnr_parameter_set_float(void*,const char*,float);
extern "C" void tf2vrnr_parameter_set_resource(void*,const char*,ID3D12Resource*);
extern "C" void tf2vrnr_parameter_set_pointer(void*,const char*,void*);
namespace {
void Check(HRESULT r,const char* name) {if(FAILED(r)){Log("fg_probe_hr %s 0x%08lx",name,(unsigned long)r);throw std::runtime_error(name);}}
void Ngx(NVSDK_NGX_Result r,const char* name) {Log("fg_probe_ngx %s 0x%08x",name,(unsigned)r);if(r!=NVSDK_NGX_Result_Success)throw std::runtime_error(name);}
template<class T>T Proc(HMODULE core,const char* name){auto p=reinterpret_cast<T>(GetProcAddress(core,name));if(!p)throw std::runtime_error(name);return p;}
struct Eye {SharedTexture color,depth,motion,output;NVSDK_NGX_Parameter* params=nullptr;NVSDK_NGX_Handle* feature=nullptr;};
}
int wmain(int argc,wchar_t** argv) {
    if(argc<2||argc>3)return 2;
    try {
        const unsigned w=argc==3?std::stoul(argv[2]):512,h=w;
        ComPtr<ID3D11Device> device;D3D_FEATURE_LEVEL level;
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,nullptr),"device");
        GpuInterop gpu;if(!gpu.Init(device.Get()))return 3;
        auto core=LoadLibraryW(L"_nvngx.dll");if(!core)throw std::runtime_error("core");
        using Init=NVSDK_NGX_Result(NVSDK_CONV*)(const char*,NVSDK_NGX_EngineType,const char*,const wchar_t*,ID3D12Device*,NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
        using Allocate=NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
        using Create=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
        using Evaluate=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback);
        using Release=NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
        using Destroy=NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
        using Shutdown=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*,unsigned*);
        auto init=Proc<Init>(core,"NVSDK_NGX_D3D12_Init_ProjectID");auto caps=Proc<Allocate>(core,"NVSDK_NGX_D3D12_GetCapabilityParameters");
        auto allocate=Proc<Allocate>(core,"NVSDK_NGX_D3D12_AllocateParameters");auto create=Proc<Create>(core,"NVSDK_NGX_D3D12_CreateFeature");
        auto evaluate=Proc<Evaluate>(core,"NVSDK_NGX_D3D12_EvaluateFeature");auto release=Proc<Release>(core,"NVSDK_NGX_D3D12_ReleaseFeature");
        auto destroy=Proc<Destroy>(core,"NVSDK_NGX_D3D12_DestroyParameters");auto shutdown=Proc<Shutdown>(core,"NVSDK_NGX_D3D12_Shutdown1");
        const wchar_t* directory=argv[1];NVSDK_NGX_FeatureCommonInfo info{};info.PathListInfo.Path=&directory;info.PathListInfo.Length=1;
        Ngx(init("0ca94998-cffe-473b-bf26-fbd774613980",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"TF2VR-FG-probe",Directory().c_str(),gpu.Device12(),NVSDK_NGX_Version_API,&info),"init");
        NVSDK_NGX_Parameter* capabilities=nullptr;Ngx(caps(&capabilities),"capabilities");
        bool available=false;
        for(const char* key:{"FrameGeneration.Available","FrameGeneration.FeatureInitResult","FrameGeneration.NeedsUpdatedDriver","FrameGeneration.MinDriverVersionMajor","FrameGeneration.MinDriverVersionMinor","FrameInterpolation.Available","FrameInterpolation.FeatureInitResult","DLSSG.MultiFrameCountMax"}) {
            int value=0;auto status=tf2vrnr_parameter_get_int(capabilities,key,&value);Log("fg_capability %s status=0x%08x value=%d hex=0x%08x",key,status,value,(unsigned)value);
            if(std::string(key)=="FrameGeneration.Available")available=status==NVSDK_NGX_Result_Success&&value;
        }
        if(!available){Ngx(destroy(capabilities),"destroy_caps");unsigned state=0;Ngx(shutdown(gpu.Device12(),&state),"shutdown");return 4;}
        std::array<Eye,2> eyes;
        for(unsigned index=0;index<2;++index) {
            auto& e=eyes[index];
            if(!gpu.CreateTexture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,e.color)||!gpu.CreateTexture(w,h,DXGI_FORMAT_R32_FLOAT,e.depth)||
               !gpu.CreateTexture(w,h,DXGI_FORMAT_R32G32_FLOAT,e.motion)||!gpu.CreateTexture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,e.output))throw std::runtime_error("texture");
            Ngx(allocate(&e.params),"allocate");auto* p=e.params;
            for(auto pair:{std::pair<const char*,unsigned>{"Width",w},{"Height",h},{"CreationNodeMask",1},{"VisibilityNodeMask",1},
                {"DLSSG.BackbufferFormat",DXGI_FORMAT_R8G8B8A8_UNORM},{"DLSSG.InternalWidth",w},{"DLSSG.InternalHeight",h},{"DLSSG.DynamicResolution",0}})
                tf2vrnr_parameter_set_uint(p,pair.first,pair.second);
            auto* list=gpu.Begin();if(!list)throw std::runtime_error("create_list");
            Ngx(create(list,NVSDK_NGX_Feature_FrameGeneration,p,&e.feature),"create");
            if(!gpu.Submit()||!gpu.Drain())throw std::runtime_error("create_submit");
            Log("fg_feature_created eye=%u handle=%p size=%ux%u",index,e.feature,w,h);
            tf2vrnr_parameter_set_resource(p,"DLSSG.Backbuffer",e.color.d12.Get());
            tf2vrnr_parameter_set_resource(p,"DLSSG.HUDLess",e.color.d12.Get());
            tf2vrnr_parameter_set_resource(p,"DLSSG.Depth",e.depth.d12.Get());
            tf2vrnr_parameter_set_resource(p,"DLSSG.MVecs",e.motion.d12.Get());
            tf2vrnr_parameter_set_resource(p,"DLSSG.OutputInterpolated",e.output.d12.Get());
            for(const char* name:{"MVecs","Depth","Backbuffer","HUDLess","OutputInterpolated"}) {
                const std::string prefix=std::string("DLSSG.")+name+"Subrect";
                tf2vrnr_parameter_set_uint(p,(prefix+"BaseX").c_str(),0);tf2vrnr_parameter_set_uint(p,(prefix+"BaseY").c_str(),0);
                tf2vrnr_parameter_set_uint(p,(prefix+"Width").c_str(),w);tf2vrnr_parameter_set_uint(p,(prefix+"Height").c_str(),h);
            }
        }
        float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        // Static perspective camera; only a foreground rectangle moves.
        const float n=.1f,f=100.f,a=f/(n-f),b=n*f/(n-f);
        float projection[16]={1,0,0,0,0,1,0,0,0,0,a,-1,0,0,b,0};
        float inverse[16]={1,0,0,0,0,1,0,0,0,0,0,1/b,0,0,-1,a/b};
        bool passed=true;
        for(unsigned frame=0;frame<8;++frame)for(unsigned eye=0;eye<2;++eye) {
            auto& e=eyes[eye];auto* p=e.params;auto* c=gpu.Context11();
            std::vector<unsigned char> color(w*h*4);std::vector<float> depth(w*h,.999f),motion(w*h*2,0);
            const unsigned left=w/4+frame*4+eye*12;
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){size_t i=size_t(y)*w+x;bool box=x>=left&&x<left+w/4&&y>=h/3&&y<h*2/3;
                auto value=static_cast<unsigned char>(box?220:20+((x/16+y/16)%2)*25);color[i*4]=value;color[i*4+1]=box?60:value;color[i*4+2]=box?40:value;color[i*4+3]=255;
                if(box){depth[i]=.98f;motion[i*2]=-4.f;}}
            c->UpdateSubresource(e.color.d11.Get(),0,nullptr,color.data(),w*4,0);c->UpdateSubresource(e.depth.d11.Get(),0,nullptr,depth.data(),w*4,0);c->UpdateSubresource(e.motion.d11.Get(),0,nullptr,motion.data(),w*8,0);
            tf2vrnr_parameter_set_pointer(p,"DLSSG.CameraViewToClip",projection);tf2vrnr_parameter_set_pointer(p,"DLSSG.ClipToCameraView",inverse);
            for(const char* key:{"DLSSG.ClipToPrevClip","DLSSG.PrevClipToClip","DLSSG.ClipToLensClip"})tf2vrnr_parameter_set_pointer(p,key,identity);
            for(auto pair:{std::pair<const char*,float>{"DLSSG.MvecScaleX",1.f},{"DLSSG.MvecScaleY",1.f},{"DLSSG.CameraNear",n},{"DLSSG.CameraFar",f},
                {"DLSSG.CameraFOV",1.5707963f},{"DLSSG.CameraAspectRatio",1.f},{"DLSSG.CameraUpY",1.f},{"DLSSG.CameraRightX",1.f},{"DLSSG.CameraFwdZ",-1.f}})
                tf2vrnr_parameter_set_float(p,pair.first,pair.second);
            for(auto pair:{std::pair<const char*,unsigned>{"DLSSG.MultiFrameCount",1},{"DLSSG.MultiFrameIndex",1},{"DLSSG.Reset",frame==0},
                {"DLSSG.CameraMotionIncluded",1},{"DLSSG.DepthInverted",0},{"DLSSG.ColorBuffersHDR",0},{"DLSSG.MenuDetectionEnabled",0}})
                tf2vrnr_parameter_set_uint(p,pair.first,pair.second);
            auto* list=gpu.Begin();if(!list)throw std::runtime_error("evaluate_list");
            for(auto* t:{&e.color,&e.depth,&e.motion})GpuInterop::Transition(list,t->d12.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            GpuInterop::Transition(list,e.output.d12.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Ngx(evaluate(list,e.feature,p,nullptr),"evaluate");
            for(auto* t:{&e.color,&e.depth,&e.motion})GpuInterop::Transition(list,t->d12.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
            GpuInterop::Transition(list,e.output.d12.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
            if(!gpu.Submit()||!gpu.Drain())throw std::runtime_error("evaluate_submit");
            D3D11_TEXTURE2D_DESC desc{};e.output.d11->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;Check(device->CreateTexture2D(&desc,nullptr,&staging),"readback");c->CopyResource(staging.Get(),e.output.d11.Get());
            D3D11_MAPPED_SUBRESOURCE map{};Check(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"map");
            std::vector<unsigned char> output(color.size());for(unsigned y=0;y<h;++y)std::memcpy(output.data()+size_t(y)*w*4,static_cast<unsigned char*>(map.pData)+size_t(y)*map.RowPitch,w*4);c->Unmap(staging.Get(),0);
            const auto path=std::filesystem::path(Directory())/("fg-"+std::to_string(frame)+"-eye"+std::to_string(eye)+".rgba");std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(output.data()),output.size());
            size_t changed=0,bright=0;double centroid=0;
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){auto i=(size_t(y)*w+x)*4;changed+=output[i]!=color[i];if(output[i]>150&&output[i+1]<100){++bright;centroid+=x;}}
            centroid/=std::max(size_t(1),bright);
            const double expected=left+(w/4-1)/2.0-(frame?2:0);
            Log("fg_output frame=%u eye=%u changed_pixels=%zu foreground_pixels=%zu centroid=%.3f expected=%.3f",frame,eye,changed,bright,centroid,expected);
            if(frame>1)passed=passed&&changed>0&&bright>100&&std::abs(centroid-expected)<1.5;
        }
        if(!gpu.Drain())throw std::runtime_error("drain");
        for(auto& e:eyes){Ngx(release(e.feature),"release");Ngx(destroy(e.params),"destroy");}
        Ngx(destroy(capabilities),"destroy_caps");unsigned state=0;Ngx(shutdown(gpu.Device12(),&state),"shutdown");FreeLibrary(core);
        Log("fg_probe_passed=%d",passed);return passed?0:5;
    }catch(const std::exception& e){Log("fg_probe_failed %s",e.what());return 1;}
}
