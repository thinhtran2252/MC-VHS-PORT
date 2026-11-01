#pragma once

#include <mutex>
#include <optional>
#include <string_view>

#include <pl/Mod.hpp>
#include "mod/Config.h"

namespace mcvhs {

class McVhsMod {
public:
    static McVhsMod& instance();
    McVhsMod();

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }
    bool load();
    bool enable();
    bool disable();
    bool unload();

private:
    bool registerMenu();
    void unregisterMenu();
    void handleToggle(bool enabled);
    void handleConfigChanged(std::string_view key, std::string_view value);
    void saveConfigLocked();

    ll::mod::NativeMod& mSelf;
    std::mutex mConfigMutex;
    ModConfig mConfig{};
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile;
    bool mMenuRegistered{};
};

} // namespace mcvhs
