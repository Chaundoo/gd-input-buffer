// Input Buffer — Geode mod for Geometry Dash 2.2081 (Geode v5)
//
// What it does:
//   When you tap the jump button, the release is held back until the press has
//   lasted at least N physics ticks. That way a slightly-early or very short tap
//   (e.g. tapping just before a cube lands, or a 1-tick wave flick) still counts.
//
//   GD simulates 240 ticks per second, so 4 ticks == 1 frame at 60 fps.
//
//   Press F6 inside a level to cycle stages:
//     STAGE 1:  2 ticks all |  2 ticks wave
//     STAGE 2:  4 ticks all |  4 ticks wave
//     STAGE 3:  8 ticks all |  8 ticks wave
//     STAGE 4: 16 ticks all |  8 ticks wave
//     STAGE 5: OFF (raw input)

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

using namespace geode::prelude;

namespace {
    struct Stage {
        int allTicks;
        int waveTicks;
    };

    constexpr std::array<Stage, 5> STAGES {{
        { 2,  2},
        { 4,  4},
        { 8,  8},
        {16,  8},
        { 0,  0}, // OFF
    }};

    // Per-player buffer state (index 0 = player 1, 1 = player 2)
    struct PlayerBuffer {
        bool held = false;           // logical state we've sent to the game
        uint64_t pressTick = 0;      // tick the current press started
        bool releasePending = false; // release is waiting for the min hold
        uint64_t releaseAtTick = 0;
    };

    int g_stage = 0;                 // index into STAGES
    uint64_t g_tick = 0;             // our own physics-step counter
    PlayerBuffer g_buf[2];
    bool g_bypass = false;           // true while we inject a release ourselves

    void resetBuffers() {
        g_tick = 0;
        g_buf[0] = {};
        g_buf[1] = {};
    }

    std::string stageText(int idx) {
        auto const& s = STAGES[idx];
        if (s.allTicks == 0) return fmt::format("STAGE {}: OFF (100% Raw Input)", idx + 1);
        return fmt::format(
            "STAGE {}: {} Ticks All | {} Ticks Wave ({:.1f}f @ 60fps)",
            idx + 1, s.allTicks, s.waveTicks, s.waveTicks / 4.0
        );
    }
}

$execute {
    auto start = Mod::get()->getSettingValue<int64_t>("default-stage");
    g_stage = std::clamp<int>(static_cast<int>(start) - 1, 0, STAGES.size() - 1);
    log::info("[InputBuffer] Loaded. Starting {}", stageText(g_stage));
    log::info("[InputBuffer] Press F6 in-level to cycle: 2/2 -> 4/4 -> 8/8 -> 16/8 -> OFF");
}

class $modify(BufferedGameLayer, GJBaseGameLayer) {
    int minTicksFor(bool isPlayer1) {
        auto const& s = STAGES[g_stage];
        PlayerObject* p = isPlayer1 ? m_player1 : m_player2;
        bool wave = p && p->m_isDart;
        return wave ? s.waveTicks : s.allTicks;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // Only buffer the main jump button. Platformer left/right pass through.
        if (g_bypass || button != 1 || STAGES[g_stage].allTicks == 0) {
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

        uint64_t minHold = static_cast<uint64_t>(this->minTicksFor(isPlayer1));
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

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (key == enumKeyCodes::KEY_F6 && isKeyDown && !isKeyRepeat && PlayLayer::get()) {
            g_stage = (g_stage + 1) % STAGES.size();

            // If we just turned it off, flush any pending releases right away.
            if (STAGES[g_stage].allTicks == 0) {
                if (auto pl = PlayLayer::get()) {
                    for (int i = 0; i < 2; i++) {
                        if (g_buf[i].releasePending) {
                            g_buf[i] = {};
                            g_bypass = true;
                            pl->handleButton(false, 1, i == 0);
                            g_bypass = false;
                        }
                    }
                }
            }

            auto text = stageText(g_stage);
            log::info("[InputBuffer] F6 Cycle -> {}", text);
            if (Mod::get()->getSettingValue<bool>("show-notifications")) {
                Notification::create(text, NotificationIcon::Info, 1.0f)->show();
            }
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};
