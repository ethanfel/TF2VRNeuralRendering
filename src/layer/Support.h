#pragma once
#include <windows.h>
#include <string>
#include <functional>
namespace tf2vrnr {
struct Config {
    bool neural = false;
    unsigned neuralResolution = 50;
    unsigned neuralIntensity = 50;
    unsigned neuralTone = 100;
    unsigned neuralDetail = 100;
    unsigned neuralPreset = 2;
    bool dlaa = false;
    bool frameGeneration = false;
    bool enabled = true;
    bool capture = true;
    bool captureFirst = false;
    bool inspectBindings = true;
    bool captureCandidates = true;
    bool depthBeforeClear = true;
    bool renderPasses = true;
    bool enginePost = false;
    unsigned burstFrames = 1;
    unsigned logEvery = 600;
};
std::wstring Directory();
Config ReadConfig();
bool IsTarget();
void Log(const char* fmt, ...);
bool ConsumeCaptureRequest();
std::wstring CaptureDirectory();
bool CaptureWriteBusy();
bool QueueCaptureWrite(std::function<void()> work);
}
