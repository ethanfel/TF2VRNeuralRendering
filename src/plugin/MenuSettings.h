#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace tf2vrnr::menu
{
// These are requested settings, not a claim that a GPU feature is active.
constexpr std::array<const char*, 8> kFields = {
    "nr", "dlaa", "fg", "model", "resolution", "strength", "tone", "detail"
};
using Settings = std::array<int, 8>;
constexpr Settings kDefaults = {0, 0, 0, 1, 50, 50, 100, 100};

inline bool Valid(const Settings& s)
{
    for (size_t i = 0; i < 4; ++i)
        if (s[i] < 0 || s[i] > 1) return false;
    if (s[4] != 25 && s[4] != 50 && s[4] != 75 && s[4] != 100) return false;
    for (size_t i = 5; i < s.size(); ++i)
        if (s[i] < 0 || s[i] > 100) return false;
    return true;
}

struct Snapshot
{
    Settings values = kDefaults;
    int runtimes = 0; // NR=1, DLAA=2, FG=4
    bool layerEnabled = true;
};

class Store
{
public:
    virtual ~Store() = default;
    virtual bool Load(Snapshot& snapshot) = 0;
    // 2=saved and reload requested; -1=write failed; -6=saved but signal failed.
    virtual int Save(const Settings& values) = 0;
};

class Variables
{
public:
    virtual ~Variables() = default;
    virtual bool Read(const std::string& name, int& value) = 0;
    virtual bool Write(const std::string& name, int value) = 0;
};

// Only an explicit request can persist settings. Defaults in mod.json never
// replace the INI. UI drafts and published saved values are separate channels.
class Controller
{
public:
    bool Tick(Variables& vars, Store& store)
    {
        int request = 0, command = 0;
        if (!vars.Read("tf2vrnr_request", request) || !vars.Read("tf2vrnr_command", command)) return false;
        if (!initialized_ || request != lastRequest_)
        {
            Snapshot disk;
            if (!store.Load(disk))
            {
                vars.Write("tf2vrnr_status", -1);
                vars.Write("tf2vrnr_ack", request);
                lastRequest_ = request;
                return false;
            }
            if (!initialized_ || disk.values != saved_) ++revision_;
            saved_ = disk.values;
            int status = 1;
            if (initialized_ && command == 2)
            {
                Settings draft;
                int base = 0;
                if (!vars.Read("tf2vrnr_base", base)) return false;
                for (size_t i = 0; i < draft.size(); ++i)
                    if (!vars.Read(std::string("tf2vrnr_edit_") + kFields[i], draft[i])) return false;
                if (base != revision_) status = -5;
                else if (!Valid(draft)) status = -3;
                else if (!disk.layerEnabled && (draft[0] || draft[1] || draft[2])) status = -4;
                else if ((draft[0] && !(disk.runtimes & 1)) || (draft[1] && !(disk.runtimes & 2)) ||
                    (draft[2] && !(disk.runtimes & 4))) status = -2;
                else
                {
                    status = store.Save(draft);
                    if (status == 2 || status == -6)
                    {
                        if (saved_ != draft) ++revision_;
                        saved_ = draft;
                    }
                }
            }
            for (size_t i = 0; i < saved_.size(); ++i)
                if (!vars.Write(std::string("tf2vrnr_saved_") + kFields[i], saved_[i])) return false;
            if (!vars.Write("tf2vrnr_revision", revision_) || !vars.Write("tf2vrnr_runtimes", disk.runtimes) ||
                !vars.Write("tf2vrnr_status", status) || !vars.Write("tf2vrnr_ready", 1) ||
                !vars.Write("tf2vrnr_ack", request)) return false;
            lastRequest_ = request;
            initialized_ = true;
        }
        return true;
    }
private:
    Settings saved_ = kDefaults;
    int revision_ = 0;
    int lastRequest_ = -1;
    bool initialized_ = false;
};
}
