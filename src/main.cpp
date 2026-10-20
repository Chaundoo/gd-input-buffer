// Input Buffer v1.1 — Geode mod for Geometry Dash 2.2081 (Geode v5)
//
// When you tap jump, the release is held back until the press has lasted at
// least N physics ticks, so slightly-early or very short taps still count.
// GD simulates 240 ticks per second: 4 ticks = 1 frame at 60 fps.
//
// Controls:
//   - Pause menu -> clock button (right side) opens the settings menu (on/off, tick counts,
//     which gamemodes are buffered). Changes apply instantly.
//   - F6 in a level toggles the buffer on/off.

#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

using namespace geode::prelude;

namespace {
    // Per-player buffer state (index 0 = player 1, 1 = player 2)
    struct PlayerBuffer {
        bool held = false;           // logical state we've sent to the game
        uint64_t pressTick = 0;      // tick the current press started
        bool releasePending = false; // release is waiting for the min hold
        uint64_t releaseAtTick = 0;
    };

    uint64_t g_tick = 0;             // our own physics-step counter
    PlayerBuffer g_buf[2];
    bool g_bypass = false;           // true while we inject a release ourselves

    template <class T>
    T setting(std::string_view key) {
        return Mod::get()->getSettingValue<T>(key);
    }

    bool enabled() { return setting<bool>("enabled"); }

    void resetBuffers() {
        g_tick = 0;
        g_buf[0] = {};
        g_buf[1] = {};
    }

    // Send any held-back releases to the game right now.
    void flushPending(GJBaseGameLayer* layer) {
        if (!layer) return;
        for (int i = 0; i < 2; i++) {
            if (g_buf[i].releasePending) {
                g_buf[i] = {};
                g_bypass = true;
                layer->handleButton(false, 1, i == 0);
                g_bypass = false;
            }
        }
    }

    // How many ticks to buffer for this player's current gamemode (0 = none).
    int minTicksFor(PlayerObject* p) {
        if (!p) return 0;
        char const* modeKey = "mode-cube";
        if (p->m_isShip)        modeKey = "mode-ship";
        else if (p->m_isBall)   modeKey = "mode-ball";
        else if (p->m_isBird)   modeKey = "mode-ufo";
        else if (p->m_isDart)   modeKey = "mode-wave";
        else if (p->m_isRobot)  modeKey = "mode-robot";
        else if (p->m_isSpider) modeKey = "mode-spider";
        else if (p->m_isSwing)  modeKey = "mode-swing";

        if (!setting<bool>(modeKey)) return 0;
        auto ticks = p->m_isDart ? setting<int64_t>("ticks-wave") : setting<int64_t>("ticks-all");
        return static_cast<int>(std::max<int64_t>(ticks, 0));
    }

    std::string statusText() {
        if (!enabled()) return "Input Buffer: OFF (raw input)";
        auto all = setting<int64_t>("ticks-all");
        auto wave = setting<int64_t>("ticks-wave");
        return fmt::format(
            "Input Buffer: ON | {} ticks all ({:.1f}f) | {} ticks wave ({:.1f}f)",
            all, all / 4.0, wave, wave / 4.0
        );
    }
}

$execute {
    log::info("[InputBuffer] Loaded. {}", statusText());
}

class $modify(BufferedGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        // Only buffer the main jump button. Platformer left/right pass through.
        if (g_bypass || button != 1 || !enabled()) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto& b = g_buf[isPlayer1 ? 0 : 1];

        if (down) {
            if (b.releasePending) {
                // Re-pressed before the buffered release fired: the game still
                // thinks we're holding, so just cancel the pending release.
                b.releasePending = false;
                b.pressTick = g_tick;
                return;
            }
            b.held = true;
            b.pressTick = g_tick;
            GJBaseGameLayer::handleButton(true, button, isPlayer1);
            return;
        }

        // Release
        if (!b.held) {
            GJBaseGameLayer::handleButton(false, button, isPlayer1);
            return;
        }

        auto minHold = static_cast<uint64_t>(minTicksFor(isPlayer1 ? m_player1 : m_player2));
        uint64_t heldFor = g_tick - b.pressTick;

        if (heldFor < minHold) {
            b.releasePending = true;
            b.releaseAtTick = b.pressTick + minHold;
            return; // swallow for now, released later in processCommands
        }

        b.held = false;
        GJBaseGameLayer::handleButton(false, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        g_tick++;

        for (int i = 0; i < 2; i++) {
            auto& b = g_buf[i];
            if (b.releasePending && g_tick >= b.releaseAtTick) {
                b.releasePending = false;
                b.held = false;
                g_bypass = true;
                this->handleButton(false, 1, i == 0);
                g_bypass = false;
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        resetBuffers();
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void resetLevel() {
        resetBuffers();
        PlayLayer::resetLevel();
    }
};

// Settings button in the pause menu
class $modify(BufferPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = this->getChildByID("right-button-menu");
        if (!menu) return;

        CCNode* sprite = CircleButtonSprite::createWithSpriteFrameName(
            "GJ_timeIcon_001.png", 1.0f, CircleBaseColor::Green, CircleBaseSize::Small
        );
        if (!sprite) {
            sprite = ButtonSprite::create("IB");
            sprite->setScale(0.7f);
        }
        auto btn = CCMenuItemSpriteExtra::create(
            sprite, this, menu_selector(BufferPauseLayer::onBufferSettings)
        );
        btn->setID("input-buffer-settings"_spr);
        menu->addChild(btn);
        menu->updateLayout();
    }

    void onBufferSettings(CCObject*) {
        openSettingsPopup(Mod::get(), false);
    }
};

// F6 = quick on/off toggle
class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (key == enumKeyCodes::KEY_F6 && isKeyDown && !isKeyRepeat && PlayLayer::get()) {
            bool now = !enabled();
            Mod::get()->setSettingValue<bool>("enabled", now);
            if (!now) flushPending(PlayLayer::get());

            auto text = statusText();
            log::info("[InputBuffer] F6 -> {}", text);
            if (setting<bool>("show-notifications")) {
                Notification::create(text, NotificationIcon::Info, 1.0f)->show();
            }
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};
