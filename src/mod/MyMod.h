#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string_view>

#include "mod/Config.h"

#include <pl/Mod.hpp>

namespace mcvhs {

class McVhsMod {
  public:
    static McVhsMod &instance();

    McVhsMod();

    [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

  private:
    void registerMenuModules();
    void unregisterMenuModules();
    void handleModuleToggle(std::string_view moduleId, bool enabled);
    void handleConfigChanged(std::string_view moduleId, std::string_view key,
                             std::string_view value);
    void mutateConfig(std::string_view key, std::string_view value);
    void refreshRuntime();

    ll::mod::NativeMod &mSelf;
    mutable std::mutex mConfigMutex;
    ModConfig mConfig;
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile;
    std::atomic_bool mLifecycleActive{false};
    bool mMenuRegistered{};
};

} // namespace mcvhs
