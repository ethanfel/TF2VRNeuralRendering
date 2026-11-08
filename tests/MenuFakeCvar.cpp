// A private VEngineCvar007 fixture. Never distribute or install this DLL.
#include <windows.h>
#include <array>
#include <cstring>
#include <map>
#include <string>

namespace
{
struct FakeVar { alignas(8) std::array<unsigned char, 0xa0> bytes{}; };
std::map<std::string, FakeVar> vars;
void* Find(void*, const char* name)
{
    auto it=vars.find(name);
    return it==vars.end() ? nullptr : it->second.bytes.data();
}
void* table[17] = {};
void** object = table;
}
extern "C" __declspec(dllexport) void* CreateInterface(const char* name, int*)
{
    table[16]=reinterpret_cast<void*>(&Find);
    return std::strcmp(name,"VEngineCvar007")==0 ? &object : nullptr;
}
extern "C" __declspec(dllexport) void Add(const char* name, void* vtable, int value)
{
    auto it=vars.emplace(name,FakeVar{}).first;
    auto p=it->second.bytes.data();
    std::memcpy(p,&vtable,sizeof(vtable));
    const char* registered=it->first.c_str();
    std::memcpy(p+0x18,&registered,sizeof(registered));
    // Avoid engine allocation/imports: this test uses integer-only convars.
    int flags=0x1000;
    std::memcpy(p+0x28,&flags,sizeof(flags));
    std::memcpy(p+0x38,&p,sizeof(p));
    std::memcpy(p+0x5c,&value,sizeof(value));
    float f=static_cast<float>(value);
    std::memcpy(p+0x58,&f,sizeof(f));
}
extern "C" __declspec(dllexport) int Get(const char* name)
{
    int result=-999;
    auto p=static_cast<unsigned char*>(Find(nullptr,name));
    if(p) std::memcpy(&result,p+0x5c,sizeof(result));
    return result;
}
extern "C" __declspec(dllexport) void Put(const char* name, int value)
{
    auto p=static_cast<unsigned char*>(Find(nullptr,name));
    if(p) std::memcpy(p+0x5c,&value,sizeof(value));
}
