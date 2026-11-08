#include "../src/layer/DepthCapture.h"
#include "../src/layer/Support.h"
#include <d3d11_4.h>
#include <wrl/client.h>
#include <thread>
using Microsoft::WRL::ComPtr;
using namespace tf2vrnr;
int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&device,nullptr,&context);
    if(FAILED(hr)){Log("threading_probe device_failed=0x%08lx",(unsigned long)hr);return 1;}
    ComPtr<ID3D11Multithread> protection;
    if(FAILED(context.As(&protection))){Log("threading_probe multithread_missing");return 1;}
    protection->SetMultithreadProtected(TRUE);
    BeginDepthCapture(device.Get(),64,64,1,true,true);
#ifdef TF2VRNR_FRAMEGEN
    Config config;config.frameGeneration=true;uint64_t ids[2]={1,2};
    ConfigureNeuralFrame(device.Get(),false,config,ids,true);
#endif
    HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread watchdog([done]{
        if(WaitForSingleObject(done,5000)!=WAIT_OBJECT_0){
            Log("threading_probe deadlock=1 context_owner_waiting_for_observer=1");
            ExitProcess(2);
        }
    });
    // The game or runtime can already own D3D11's recursive context lock.
    // A simultaneous observer must acquire that lock BEFORE its own mutex.
    protection->Enter();
    std::thread other([&]{context->OMSetRenderTargets(0,nullptr,nullptr);});
    Sleep(300); // Let the competing observer reach the held context lock.
    Log("threading_probe owner_call_begin");
    context->OMSetRenderTargets(0,nullptr,nullptr);
    protection->Leave();
    other.join();
    EndDepthCapture(device.Get());
#ifdef TF2VRNR_FRAMEGEN
    DestroyNeuralBackend(device.Get());
#endif
    SetEvent(done);watchdog.join();CloseHandle(done);
    Log("threading_probe passed=1 both_threads_completed=1");
    return 0;
}
