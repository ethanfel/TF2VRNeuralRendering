#include "MenuBridge.h"
#include "MenuConfig.h"
#include <windows.h>
#include <cstring>
#include <map>
#include <memory>

namespace tf2vrnr::menu
{
namespace
{
template<class T> bool ReadMemory(const void* source, T& value)
{
    SIZE_T bytes = 0;
    return ReadProcessMemory(GetCurrentProcess(), source, &value, sizeof(value), &bytes) && bytes == sizeof(value);
}
bool InModule(const void* address, HMODULE module)
{
    MEMORY_BASIC_INFORMATION info = {};
    return module && VirtualQuery(address, &info, sizeof(info)) && info.AllocationBase == module &&
        info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}

// Northstar VEngineCvar007 FindVar=16; ConVar parent=0x38, integer=0x5c.
// The installed engine's ConVar vtable slot 14 is SetValue(int), RVA 0x417cf0.
// Verify its integer-specific prologue before calling it. An engine ABI change
// disables the bridge, rather than calling an unverified virtual function.
// All calls run from Northstar's main-thread RunFrame callback, on our own vars.
class GameVariables : public Variables
{
public:
    bool Read(const std::string& name, int& value) override
    {
        auto var = Resolve(name);
        return var && ReadMemory(var + 0x5c, value);
    }
    bool Write(const std::string& name, int value) override
    {
        auto var = Resolve(name);
        if (!var) return false;
        void** table = nullptr;
        using SetInt = void (*)(void*, int);
        SetInt set = nullptr;
        if (!ReadMemory(var, table) || !ReadMemory(table + 14, set) || !InModule(reinterpret_cast<void*>(set), engine_)) return false;
        constexpr unsigned char signature[] = {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x50,
            0x8b,0xfa,0x48,0x8b,0xd9,0x3b,0x51,0x5c};
        unsigned char actual[sizeof(signature)] = {};
        if (!ReadMemory(reinterpret_cast<void*>(set), actual) || std::memcmp(actual, signature, sizeof(actual))) return false;
        set(var, value);
        int result = 0;
        return ReadMemory(var + 0x5c, result) && result == value;
    }
private:
    unsigned char* Resolve(const std::string& name)
    {
        if (name.compare(0, 8, "tf2vrnr_") != 0) return nullptr;
        const auto found = vars_.find(name);
        if (found != vars_.end()) return found->second;
        HMODULE vstdlib = GetModuleHandleW(L"vstdlib.dll");
        engine_ = GetModuleHandleW(L"engine.dll");
        if (!vstdlib || !engine_) return nullptr;
        using Factory = void* (*)(const char*, int*);
        auto factory = reinterpret_cast<Factory>(GetProcAddress(vstdlib, "CreateInterface"));
        if (!factory) return nullptr;
        void* cvar = factory("VEngineCvar007", nullptr);
        void** table = nullptr;
        using FindVar = void* (*)(void*, const char*);
        FindVar find = nullptr;
        if (!ReadMemory(cvar, table) || !InModule(table, vstdlib) || !ReadMemory(table + 16, find) ||
            !InModule(reinterpret_cast<void*>(find), vstdlib)) return nullptr;
        auto var = static_cast<unsigned char*>(find(cvar, name.c_str()));
        const char* registered = nullptr;
        unsigned char* parent = nullptr;
        if (!var || !ReadMemory(var, table) || !InModule(table, engine_) ||
            !ReadMemory(var + 0x18, registered) || !ReadMemory(var + 0x38, parent) || parent != var) return nullptr;
        for (size_t i = 0; i <= name.size(); ++i)
        {
            char c = 0;
            if (!ReadMemory(registered + i, c) || c != name.c_str()[i]) return nullptr;
        }
        vars_[name] = var;
        return var;
    }
    std::map<std::string, unsigned char*> vars_;
    HMODULE engine_ = nullptr;
};
GameVariables variables;
Controller controller;
std::unique_ptr<IniStore> store;
ULONGLONG nextPoll = 0;
}

void Initialize(const std::wstring& layerDirectory)
{
    store = std::make_unique<IniStore>(layerDirectory);
    controller = Controller{};
    nextPoll = 0;
}
void RunFrame()
{
    const auto now = GetTickCount64();
    if (!store || now < nextPoll) return;
    nextPoll = now + 250;
    controller.Tick(variables, *store);
}
}
