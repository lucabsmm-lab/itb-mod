#include "UI.hpp"

#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>

namespace {
    constexpr char const* kPositions[] = { "Top Left", "Top Right", "Bottom Left", "Bottom Right" };

    // -----------------------------------------------------------------------
    // On-screen indicator
    // -----------------------------------------------------------------------
    class IndicatorNode : public CCNode {
        CCLabelBMFont* m_label = nullptr;
        std::string m_text;

    public:
        static IndicatorNode* create() {
            auto ret = new IndicatorNode();
            if (ret->init()) {
                ret->autorelease();
                return ret;
            }
            delete ret;
            return nullptr;
        }

        bool init() override {
            if (!CCNode::init()) return false;
            m_label = CCLabelBMFont::create("", "bigFont.fnt");
            m_label->setScale(.4f);
            m_label->setOpacity(210);
            this->addChild(m_label);
            this->scheduleUpdate();
            this->update(0.f);
            return true;
        }

        void update(float dt) override {
            auto& rt = runtime();
            auto const& c = config();
            if (rt.bannerTimer > 0.f) rt.bannerTimer -= dt;
            if (rt.flashTimer > 0.f) rt.flashTimer -= dt;

            this->setVisible(c.showIndicator);
            if (!c.showIndicator) return;

            std::string text;
            if (!c.enabled) text = "OFF";
            else if (rt.bannerTimer > 0.f) text = "ON";
            else text = fmt::format("{} ticks", c.bufferTicks);
            if (text != m_text) {
                m_text = text;
                m_label->setString(m_text.c_str());
            }

            if (!c.enabled) m_label->setColor({ 255, 90, 90 });
            else if (rt.flashTimer > 0.f) m_label->setColor({ 110, 255, 120 });
            else m_label->setColor({ 255, 255, 255 });

            auto win = CCDirector::get()->getWinSize();
            constexpr float m = 5.f;
            auto const& pos = c.indicatorPos;
            bool right = pos == "Top Right" || pos == "Bottom Right";
            bool bottom = pos == "Bottom Left" || pos == "Bottom Right";
            m_label->setAnchorPoint({ right ? 1.f : 0.f, bottom ? 0.f : 1.f });
            m_label->setPosition({ right ? win.width - m : m, bottom ? m : win.height - m });
        }
    };

    // -----------------------------------------------------------------------
    // Settings popup
    // -----------------------------------------------------------------------
    class SettingsPopup : public Popup {
    protected:
        std::vector<CCMenuItemSpriteExtra*> m_tickButtons;
        ButtonSprite* m_posSprite = nullptr;
        ButtonSprite* m_styleSprite = nullptr;

        CCMenuItemToggler* addToggle(char const* text, bool on, SEL_MenuHandler callback, CCPoint offset) {
            auto toggle = CCMenuItemToggler::createWithStandardSprites(this, callback, .6f);
            toggle->toggle(on);
            m_buttonMenu->addChildAtPosition(toggle, Anchor::Top, offset);

            auto label = CCLabelBMFont::create(text, "bigFont.fnt");
            label->setScale(.42f);
            label->setAnchorPoint({ 0.f, .5f });
            m_mainLayer->addChildAtPosition(label, Anchor::Top, offset + CCPoint { 18.f, 0.f });
            return toggle;
        }

        void addHeading(char const* text, CCPoint offset, float scale = .45f) {
            auto label = CCLabelBMFont::create(text, "goldFont.fnt");
            label->setScale(scale);
            m_mainLayer->addChildAtPosition(label, Anchor::Top, offset);
        }

        ButtonSprite* addCycleButton(std::string const& text, SEL_MenuHandler callback, CCPoint offset) {
            auto spr = ButtonSprite::create(text.c_str(), 120, true, "bigFont.fnt", "GJ_button_05.png", 28.f, .5f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, callback);
            m_buttonMenu->addChildAtPosition(btn, Anchor::Top, offset);
            return spr;
        }

        bool init() {
            if (!Popup::init(360.f, 260.f)) return false;
            this->setTitle("Input Buffer");
            auto const& c = config();

            addToggle("Enabled", c.enabled, menu_selector(SettingsPopup::onEnabled), { -150.f, -52.f });
            auto hint = CCLabelBMFont::create("F1 = on   F2 = off", "chatFont.fnt");
            hint->setScale(.7f);
            hint->setOpacity(180);
            m_mainLayer->addChildAtPosition(hint, Anchor::Top, { 90.f, -52.f });

            addHeading("Buffer Ticks", { 0.f, -86.f });
            for (int i = 0; i < 8; ++i) {
                int value = 2 + i * 2;
                auto spr = ButtonSprite::create(std::to_string(value).c_str(), 24, true, "bigFont.fnt", "GJ_button_04.png", 26.f, .6f);
                auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(SettingsPopup::onTicks));
                btn->setTag(value);
                m_buttonMenu->addChildAtPosition(btn, Anchor::Top, { -140.f + i * 40.f, -114.f });
                m_tickButtons.push_back(btn);
            }
            auto keys = CCLabelBMFont::create("Hotkeys: 0+2  0+4  0+6  0+8  1+0  1+2  1+4  1+6", "chatFont.fnt");
            keys->setScale(.6f);
            keys->setOpacity(180);
            m_mainLayer->addChildAtPosition(keys, Anchor::Top, { 0.f, -138.f });
            updateTicks();

            addToggle("Safe Mode", c.safeMode, menu_selector(SettingsPopup::onSafeMode), { -150.f, -166.f });
            addToggle("Indicator", c.showIndicator, menu_selector(SettingsPopup::onIndicator), { 30.f, -166.f });

            addHeading("Indicator Spot", { -80.f, -196.f }, .4f);
            m_posSprite = addCycleButton(c.indicatorPos, menu_selector(SettingsPopup::onPosition), { -80.f, -222.f });
            addHeading("Timing", { 80.f, -196.f }, .4f);
            m_styleSprite = addCycleButton(c.safest ? "Safest" : "Earliest", menu_selector(SettingsPopup::onStyle), { 80.f, -222.f });

            auto moreSpr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
            moreSpr->setScale(.5f);
            auto more = CCMenuItemSpriteExtra::create(moreSpr, this, menu_selector(SettingsPopup::onMore));
            m_buttonMenu->addChildAtPosition(more, Anchor::TopRight, { -22.f, -22.f });
            return true;
        }

        void updateTicks() {
            for (auto btn : m_tickButtons) {
                if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                    spr->updateBGImage(btn->getTag() == config().bufferTicks ? "GJ_button_01.png" : "GJ_button_04.png");
                }
            }
        }

        static bool newToggleState(CCObject* sender) {
            // The toggler flips after the callback runs.
            return !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        }

        void onEnabled(CCObject* sender) {
            setEnabled(newToggleState(sender));
            announceState(false);
        }

        void onSafeMode(CCObject* sender) {
            Mod::get()->setSettingValue<bool>("safe-mode", newToggleState(sender));
        }

        void onIndicator(CCObject* sender) {
            Mod::get()->setSettingValue<bool>("show-indicator", newToggleState(sender));
        }

        void onTicks(CCObject* sender) {
            setBufferTicks(sender->getTag());
            updateTicks();
        }

        void onPosition(CCObject*) {
            auto const& current = config().indicatorPos;
            size_t next = 0;
            for (size_t i = 0; i < 4; ++i) {
                if (current == kPositions[i]) next = (i + 1) % 4;
            }
            Mod::get()->setSettingValue<std::string>("indicator-position", kPositions[next]);
            m_posSprite->setString(kPositions[next]);
        }

        void onStyle(CCObject*) {
            std::string next = config().safest ? "Earliest" : "Safest";
            Mod::get()->setSettingValue<std::string>("timing-style", next);
            m_styleSprite->setString(next.c_str());
        }

        void onMore(CCObject*) {
            openSettingsPopup(Mod::get());
        }

    public:
        static SettingsPopup* create() {
            auto ret = new SettingsPopup();
            if (ret->init()) {
                ret->autorelease();
                return ret;
            }
            delete ret;
            return nullptr;
        }
    };
}

namespace ui {
    void addIndicator(PlayLayer* layer) {
        auto node = IndicatorNode::create();
        node->setID("indicator"_spr);
        CCNode* parent = layer->m_uiLayer ? static_cast<CCNode*>(layer->m_uiLayer) : layer;
        parent->addChild(node, 1000);
    }

    void openSettings() {
        if (auto popup = SettingsPopup::create()) popup->show();
    }

    CCMenuItemSpriteExtra* makeOpenButton(CCObject* target, SEL_MenuHandler callback, float scale) {
        auto label = CCLabelBMFont::create("IB", "bigFont.fnt");
        auto spr = CircleButtonSprite::create(label, CircleBaseColor::Green, CircleBaseSize::Medium);
        spr->setScale(scale);
        auto btn = CCMenuItemSpriteExtra::create(spr, target, callback);
        btn->setID("open-button"_spr);
        return btn;
    }

    void addOpenButton(CCNode* layer, char const* menuID, CCMenuItemSpriteExtra* button, CCPoint fallbackPos) {
        if (auto menu = layer->getChildByID(menuID)) {
            menu->addChild(button);
            menu->updateLayout();
            return;
        }
        auto menu = CCMenu::create();
        menu->setID("menu"_spr);
        menu->setPosition(fallbackPos);
        menu->addChild(button);
        layer->addChild(menu, 10);
    }
}

// ---------------------------------------------------------------------------
// Buttons: pause menu, online/saved level page, "My Levels" level page
// ---------------------------------------------------------------------------

class $modify(IBPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto win = CCDirector::get()->getWinSize();
        ui::addOpenButton(
            this, "right-button-menu",
            ui::makeOpenButton(this, menu_selector(IBPauseLayer::onInputBuffer), .7f),
            { win.width - 36.f, win.height / 2.f - 90.f }
        );
    }

    void onInputBuffer(CCObject*) {
        ui::openSettings();
    }
};

class $modify(IBLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        auto win = CCDirector::get()->getWinSize();
        ui::addOpenButton(
            this, "left-side-menu",
            ui::makeOpenButton(this, menu_selector(IBLevelInfoLayer::onInputBuffer), .8f),
            { 30.f, win.height / 2.f - 110.f }
        );
        return true;
    }

    void onInputBuffer(CCObject*) {
        ui::openSettings();
    }
};

class $modify(IBEditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;
        auto win = CCDirector::get()->getWinSize();
        ui::addOpenButton(
            this, "level-actions-menu",
            ui::makeOpenButton(this, menu_selector(IBEditLevelLayer::onInputBuffer), .8f),
            { win.width - 30.f, 40.f }
        );
        return true;
    }

    void onInputBuffer(CCObject*) {
        ui::openSettings();
    }
};
