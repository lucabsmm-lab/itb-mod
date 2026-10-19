#include "State.hpp"

BufferConfig& ibConfig() {
    static BufferConfig c;
    return c;
}

BufferRuntime& ibRuntime() {
    static BufferRuntime r;
    return r;
}

void BufferRuntime::clearQueues() {
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
    ibConfig().enabled = on;
}

void setBufferTicks(int ticks) {
    ticks = snapTicks(ticks);
    Mod::get()->setSettingValue<int64_t>("buffer-ticks", ticks);
    ibConfig().bufferTicks = ticks;
}

void announceState(bool alsoNotify) {
    ibRuntime().bannerTimer = 2.f;
    if (alsoNotify && !PlayLayer::get()) {
        auto& c = ibConfig();
        auto text = c.enabled
            ? fmt::format("Input Buffer: ON ({} ticks)", c.bufferTicks)
            : std::string("Input Buffer: OFF");
        Notification::create(text, NotificationIcon::Info, 1.5f)->show();
    }
}

static void loadAll() {
    auto mod = Mod::get();
    auto& c = ibConfig();
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
    c.timeLimitMs = static_cast<int>(mod->getSettingValue<int64_t>("time-limit"));
    c.debug = mod->getSettingValue<bool>("debug");
}

$on_mod(Loaded) {
    loadAll();
    for (auto key : {"timing-style", "indicator-position"}) {
        listenForSettingChanges<std::string>(key, [](std::string) { loadAll(); });
    }
    for (auto key : {"enabled", "safe-mode", "show-indicator", "hotkeys", "debug"}) {
        listenForSettingChanges<bool>(key, [](bool) { loadAll(); });
    }
    for (auto key : {"buffer-ticks", "assumed-hold", "fly-window", "sim-budget", "time-limit"}) {
        listenForSettingChanges<int64_t>(key, [](int64_t) { loadAll(); });
    }
}
