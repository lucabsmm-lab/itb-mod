#include "State.hpp"

Config& config() {
    static Config c;
    return c;
}

Runtime& runtime() {
    static Runtime r;
    return r;
}

void Runtime::clearQueues() {
    for (auto& ch : channels) {
        ch.queue.clear();
        ch.lastShift = 0;
    }
}

static int snapTicks(int ticks) {
    ticks = std::clamp(ticks, 2, 16);
    return ticks - (ticks % 2);
}

void setEnabled(bool on) {
    Mod::get()->setSettingValue<bool>("enabled", on);
    config().enabled = on;
}

void setBufferTicks(int ticks) {
    ticks = snapTicks(ticks);
    Mod::get()->setSettingValue<int64_t>("buffer-ticks", ticks);
    config().bufferTicks = ticks;
}

void announceState(bool alsoNotify) {
    runtime().bannerTimer = 2.f;
    if (alsoNotify && !PlayLayer::get()) {
        auto& c = config();
        auto text = c.enabled
            ? fmt::format("Input Buffer: ON ({} ticks)", c.bufferTicks)
            : std::string("Input Buffer: OFF");
        Notification::create(text, NotificationIcon::Info, 1.5f)->show();
    }
}

static void loadAll() {
    auto mod = Mod::get();
    auto& c = config();
    c.enabled = mod->getSettingValue<bool>("enabled");
    c.bufferTicks = snapTicks(static_cast<int>(mod->getSettingValue<int64_t>("buffer-ticks")));
    c.safest = mod->getSettingValue<std::string>("timing-style") != "Earliest";
    c.safeMode = mod->getSettingValue<bool>("safe-mode");
    c.showIndicator = mod->getSettingValue<bool>("show-indicator");
    c.indicatorPos = mod->getSettingValue<std::string>("indicator-position");
    c.hotkeys = mod->getSettingValue<bool>("hotkeys");
    c.assumedHold = static_cast<int>(mod->getSettingValue<int64_t>("assumed-hold"));
    c.flyWindow = static_cast<int>(mod->getSettingValue<int64_t>("fly-window"));
    c.simBudget = static_cast<int>(mod->getSettingValue<int64_t>("sim-budget"));
}

$on_mod(Loaded) {
    loadAll();
    for (auto key : {"timing-style", "indicator-position"}) {
        listenForSettingChanges<std::string>(key, [](std::string) { loadAll(); });
    }
    for (auto key : {"enabled", "safe-mode", "show-indicator", "hotkeys"}) {
        listenForSettingChanges<bool>(key, [](bool) { loadAll(); });
    }
    for (auto key : {"buffer-ticks", "assumed-hold", "fly-window", "sim-budget"}) {
        listenForSettingChanges<int64_t>(key, [](int64_t) { loadAll(); });
    }
}
