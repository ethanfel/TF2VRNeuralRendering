// MIT. Compile with clang-cl: NGX's overloaded virtual parameter interface
// must use the MSVC C++ ABI, even when the diagnostic host uses MinGW.
using size_t = decltype(sizeof(0));
#include <nvsdk_ngx_params.h>
extern "C" unsigned int tf2vrnr_parameter_get_int(void* parameters, const char* key, int* value) {
    return static_cast<unsigned int>(static_cast<NVSDK_NGX_Parameter*>(parameters)->Get(key, value));
}
extern "C" void tf2vrnr_parameter_set_int(void* p, const char* key, int value) {
    static_cast<NVSDK_NGX_Parameter*>(p)->Set(key, value);
}
extern "C" void tf2vrnr_parameter_set_uint(void* p, const char* key, unsigned int value) {
    static_cast<NVSDK_NGX_Parameter*>(p)->Set(key, value);
}
extern "C" void tf2vrnr_parameter_set_float(void* p, const char* key, float value) {
    static_cast<NVSDK_NGX_Parameter*>(p)->Set(key, value);
}
extern "C" void tf2vrnr_parameter_set_resource(void* p, const char* key, ID3D12Resource* value) {
    static_cast<NVSDK_NGX_Parameter*>(p)->Set(key, value);
}
extern "C" void tf2vrnr_parameter_set_pointer(void* p, const char* key, void* value) {
    static_cast<NVSDK_NGX_Parameter*>(p)->Set(key, value);
}
