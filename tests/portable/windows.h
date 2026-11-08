#pragma once
// Minimal host shim for testing actual layer dispatch without executing Wine.
// This directory is NEVER on a Windows DLL target's include path.
#include <chrono>
#include <cstdio>
#include <unistd.h>
#define __declspec(x)
inline unsigned long GetCurrentProcessId() { return static_cast<unsigned long>(getpid()); }
inline unsigned long long GetTickCount64() {
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// The mock capture sink writes no real files; metadata file creation is skipped.
inline FILE* _wfopen(const wchar_t*, const wchar_t*) { return nullptr; }

inline bool DeleteFileW(const wchar_t*) { return false; }
