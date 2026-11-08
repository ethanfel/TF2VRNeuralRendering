// Real D3D11 hooks and engine-post signature, in an isolated standalone process.
#include "../src/layer/DepthCapture.h"
#include "../src/layer/Support.h"
#include "../src/backend/FrameGeneration.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <stdexcept>
#include <cstring>
using namespace tf2vrnr;using Microsoft::WRL::ComPtr;
static void D(HRESULT r){if(FAILED(r))throw std::runtime_error("d3d11");}
int main(int argc,char** argv){
    try{
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        constexpr unsigned w=512,h=512;
        struct Target{ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11ShaderResourceView> srv;}hdr,motion,exposure,output;
        auto make=[&](Target& t,DXGI_FORMAT format,unsigned width,unsigned height){D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;D(device->CreateTexture2D(&d,nullptr,&t.texture));D(device->CreateRenderTargetView(t.texture.Get(),nullptr,&t.rtv));D(device->CreateShaderResourceView(t.texture.Get(),nullptr,&t.srv));};
        make(hdr,DXGI_FORMAT_R16G16B16A16_FLOAT,w,h);make(motion,DXGI_FORMAT_R16G16_FLOAT,w,h);make(exposure,DXGI_FORMAT_R16_FLOAT,1,1);make(output,DXGI_FORMAT_R8G8B8A8_UNORM,w,h);
        D3D11_TEXTURE2D_DESC dd{};dd.Width=w;dd.Height=h;dd.MipLevels=dd.ArraySize=dd.SampleDesc.Count=1;dd.Format=DXGI_FORMAT_R24G8_TYPELESS;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> depth;D(device->CreateTexture2D(&dd,nullptr,&depth));D3D11_DEPTH_STENCIL_VIEW_DESC ds{};ds.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;ds.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;ComPtr<ID3D11DepthStencilView> dsv;D(device->CreateDepthStencilView(depth.Get(),&ds,&dsv));
        const char* vsText="float4 main(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
        const char* psText="cbuffer CBufEnginePost:register(b1){float4 params[19];};Texture2D<float4> FBTexture:register(t1);Texture2D<float> Exposure:register(t30);float4 main(float4 p:SV_Position):SV_Target{return FBTexture.Load(int3(p.xy,0))*Exposure.Load(int3(0,0,0))*params[0].x;}";
        ComPtr<ID3DBlob> vsCode,psCode,error;D(D3DCompile(vsText,strlen(vsText),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vsCode,&error));D(D3DCompile(psText,strlen(psText),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&psCode,&error));
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;D(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));D(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
        auto buffer=[&](unsigned size){D3D11_BUFFER_DESC d{};d.ByteWidth=size;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> b;D(device->CreateBuffer(&d,nullptr,&b));return b;};auto common=buffer(576),post=buffer(304);
        Config config;config.frameGeneration=true;config.neural=false;config.dlaa=false;uint64_t ids[2]={1,2};
        if(argc==2&&!std::strcmp(argv[1],"--combined")){config.neural=true;config.dlaa=true;config.neuralResolution=config.neuralIntensity=100;}
        for(unsigned frame=0;frame<6;++frame){
            // Reconfigure NR while FG remains live: its feature handles must survive.
            if(config.dlaa&&frame==3)config.neuralTone=50;
            if(config.dlaa&&frame==4)config.neural=false;
            if(config.dlaa&&frame==5)config.neural=true;
            BeginDepthCapture(device.Get(),w,h,frame,true,true);ConfigureNeuralFrame(device.Get(),false,config,ids,frame==0);
            const float tangents[2][4]={{-1,1,1,-1},{-1,1,1,-1}};SetFrameCameraProjection(device.Get(),tangents);
            if(config.dlaa&&!SetNeuralJitter(device.Get(),0,0))throw std::runtime_error("jitter");
            for(unsigned eye=0;eye<2;++eye){
                context->ClearState();float color[4]={.3f+eye*.2f,.4f,.2f,1},zero[4]{},one[4]={1,0,0,0};
                context->ClearRenderTargetView(hdr.rtv.Get(),color);context->ClearRenderTargetView(motion.rtv.Get(),zero);context->ClearRenderTargetView(exposure.rtv.Get(),one);context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,.98f,0);
                float camera[144]{};camera[0]=1;camera[4]=camera[9]=camera[14]=camera[18]=1;camera[15]=-1;camera[114]=w;camera[115]=h;camera[135]=camera[136]=camera[137]=1;
                camera[0]=16;camera[114]=1920;camera[115]=1080;camera[100]=INFINITY; // Stale desktop metadata is not the XR eye projection.
                if(frame==2&&eye==1)camera[19]=1; // Deliberately invalid post/UI camera: must fail closed.
                context->UpdateSubresource(common.Get(),0,nullptr,camera,0,0);ID3D11Buffer* cb=common.Get();context->PSSetConstantBuffers(2,1,&cb);
                D3D11_VIEWPORT vp{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&vp);context->VSSetShader(vs.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D11RenderTargetView* scene[]={hdr.rtv.Get(),motion.rtv.Get()};context->OMSetRenderTargets(2,scene,dsv.Get());context->Draw(3,0);
                ID3D11RenderTargetView* out=output.rtv.Get();context->OMSetRenderTargets(1,&out,nullptr);context->PSSetShader(ps.Get(),nullptr,0);
                ID3D11ShaderResourceView* hs=hdr.srv.Get();ID3D11ShaderResourceView* es=exposure.srv.Get();context->PSSetShaderResources(1,1,&hs);context->PSSetShaderResources(30,1,&es);
                float parameters[76]{};parameters[0]=1;context->UpdateSubresource(post.Get(),0,nullptr,parameters,0,0);cb=post.Get();context->PSSetConstantBuffers(1,1,&cb);context->Draw(3,0);
                bool generated=GenerateReleasedView(device.Get(),ids[eye],output.texture.Get());if(generated!=(frame!=2||eye==0))throw std::runtime_error("camera_or_guide_capture");
                if(!ReleaseNeuralView(device.Get(),ids[eye]))throw std::runtime_error("release");
            }
            auto pair=GeneratedStereoFrame(device.Get());if(bool(pair)!=(frame!=2))throw std::runtime_error("stereo_freshness");
            Log("fg_capture_frame frame=%u pair=%d",frame,bool(pair));
        }
        EndDepthCapture(device.Get());DestroyNeuralBackend(device.Get());Log("fg_capture_probe_passed=1 invalid_camera_rejected=1");return 0;
    }catch(const std::exception& e){Log("fg_capture_probe_failed %s",e.what());return 1;}
}
