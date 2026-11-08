#pragma once

#include <windows.h>
#include <cstdint>

// DLL boundary types must have external linkage. In an anonymous namespace,
// GCC can conclude that ISys has no implementations and replace Log's virtual
// dispatch with __cxa_pure_virtual. The implementation lives in Northstar.dll.
// Keep the NSSys001 / Plugin*001 vtable order; do not add virtual destructors.
namespace tf2vrnr::northstar
{
enum class PluginString : int { Name = 0, LogName = 1, DependencyName = 2 };
enum class PluginField : int { Context = 0, Color = 1 };
enum class LogLevel : int { Info = 0, Warn = 1, Error = 2 };

struct PluginNorthstarData
{
    HMODULE pluginHandle;
    uint64_t size;
};

class IPluginId
{
public:
    virtual const char* GetString(PluginString prop) = 0;
    virtual int64_t GetField(PluginField prop) = 0;
};

class IPluginCallbacks
{
public:
    virtual void Init(HMODULE northstarModule, const PluginNorthstarData* initData, bool reloaded) = 0;
    virtual void Finalize() = 0;
    virtual bool Unload() = 0;
    virtual void OnSqvmCreated(void* sqvm) = 0;
    virtual void OnSqvmDestroying(void* sqvm) = 0;
    virtual void OnLibraryLoaded(HMODULE module, const char* name) = 0;
    virtual void RunFrame() = 0;
};

class ISys
{
public:
    virtual void Log(int64_t unused, LogLevel level, char* msg) = 0;
    virtual void Unload(int64_t unused) = 0;
    virtual void Reload(int64_t unused) = 0;
};

using CreateInterfaceFn = void* (*)(const char* name, int* status);
}
