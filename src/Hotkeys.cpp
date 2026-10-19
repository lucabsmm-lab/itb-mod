#include "State.hpp"

#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <array>

namespace {
    std::array<bool, 10> g_digitHeld {};

    // 0-9 on the number row or the numpad, -1 otherwise.
    int digitOf(enumKeyCodes key) {
        int code = static_cast<int>(key);
        if (code >= KEY_Zero && code <= KEY_Nine) return code - KEY_Zero;
        if (code >= KEY_NumPad0 && code <= KEY_NumPad9) return code - KEY_NumPad0;
        return -1;
    }

    // Two digits pressed together -> ticks. 0+2 = 2 ... 0+8 = 8, 1+0 = 10 ... 1+6 = 16.
    int ticksForPair(int a, int b) {
        if (a > b) std::swap(a, b);
        if (a == 0 && (b == 2 || b == 4 || b == 6 || b == 8)) return b;
        if (a == 0 && b == 1) return 10;
        if (a == 1 && (b == 2 || b == 4 || b == 6)) return 10 + b;
        return 0;
    }

    void onKey(enumKeyCodes key, bool down, bool repeat) {
        int digit = digitOf(key);
        if (digit >= 0) {
            bool wasHeld = g_digitHeld[digit];
            g_digitHeld[digit] = down;
            if (!down || wasHeld || repeat) return;
        }
        if (!down || repeat || !ibConfig().hotkeys) return;

        if (key == KEY_F1) {
            setEnabled(true);
            announceState(true);
            return;
        }
        if (key == KEY_F2) {
            setEnabled(false);
            announceState(true);
            return;
        }
        if (digit < 0) return;

        for (int other = 0; other < 10; ++other) {
            if (other == digit || !g_digitHeld[other]) continue;
            if (int ticks = ticksForPair(digit, other)) {
                setBufferTicks(ticks);
                ibRuntime().flashTimer = .35f;
                if (!PlayLayer::get()) {
                    Notification::create(fmt::format("Input Buffer: {} ticks", ticks), NotificationIcon::Info, 1.2f)->show();
                }
                return;
            }
        }
    }
}

class $modify(IBKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        onKey(key, down, repeat);
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};
