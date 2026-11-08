#pragma once
#include "MenuSettings.h"
#include <string>

namespace tf2vrnr::menu
{
class IniStore : public Store
{
public:
    explicit IniStore(std::wstring directory) : directory_(std::move(directory)) {}
    bool Load(Snapshot& snapshot) override;
    int Save(const Settings& values) override;
private:
    std::wstring directory_;
};
}
