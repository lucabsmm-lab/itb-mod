#pragma once

#include "State.hpp"

namespace ui {
    void addIndicator(PlayLayer* layer);
    void openSettings();
    // The round "IB" button that opens the settings popup.
    CCMenuItemSpriteExtra* makeOpenButton(CCObject* target, cocos2d::SEL_MenuHandler callback, float scale);
    // Adds the button to the menu with this node ID (falls back to a menu of our own).
    void addOpenButton(CCNode* layer, char const* menuID, CCMenuItemSpriteExtra* button, CCPoint fallbackPos);
}
