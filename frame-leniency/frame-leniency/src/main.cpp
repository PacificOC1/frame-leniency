#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>

#include <algorithm>
#include <vector>

using namespace geode::prelude;

// Timing: GD 2.2 runs physics at 240 steps per second, and
// GJBaseGameLayer::processCommands runs once per step. We count those
// steps ourselves (s_step) rather than trusting any of the game's own
// counters, and every window below is measured in them.

namespace {
    // Hazard grace: a run of consecutive steps in which the player was
    // touching something deadly.
    struct GraceState {
        bool active = false;
        unsigned start = 0;               // step the run began
        unsigned lastReport = 0;          // last step the game reported a hit
        bool autoReleased = false;        // we let go of the button during this run
        std::vector<GameObject*> hazards; // what we've forgiven during this run
    };

    // Everything about one side's jump button (player 1, or player 2 in
    // 2-player mode).
    struct ButtonState {
        bool held = false;               // the real button is down
        bool consumed = false;           // this click already made the player do something
        bool pendingRelease = false;     // input buffer: real button is up, we're still holding it
        unsigned releaseAt = 0;
        bool autoReleaseQueued = false;  // auto-release: let go at the start of next step
        bool autoReleased = false;       // auto-release: we let go while the real button is still down
    };

    unsigned s_step = 0;
    GraceState s_grace[2];
    ButtonState s_button[2];
    bool s_injecting = false;  // our own fake press/release is going through
    bool s_forceKill = false;  // our own "grace ran out" death is going through

    // "Saved" counters for the on-screen label (reset each attempt).
    int s_graceSaves = 0;
    int s_bufferSaves = 0;
    int s_releaseSaves = 0;
    bool s_labelDirty = true;

    bool boolSetting(char const* key) {
        return Mod::get()->getSettingValue<bool>(key);
    }

    unsigned stepSetting(char const* key) {
        auto value = Mod::get()->getSettingValue<int64_t>(key);
        return value < 0 ? 0u : static_cast<unsigned>(value);
    }

    bool isTwoPlayer(GJBaseGameLayer* layer) {
        return layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
    }

    void resetState() {
        for (auto& g : s_grace) g = {};
        for (auto& b : s_button) b = {};
        s_graceSaves = 0;
        s_bufferSaves = 0;
        s_releaseSaves = 0;
        s_labelDirty = true;
    }

    // Sends a press or release straight to the game, skipping our own hooks.
    void inject(GJBaseGameLayer* layer, int index, bool down) {
        s_injecting = true;
        layer->handleButton(down, static_cast<int>(PlayerButton::Jump), index == 0);
        s_injecting = false;
    }

    void sendBufferedRelease(GJBaseGameLayer* layer, int index) {
        s_button[index].pendingRelease = false;
        inject(layer, index, false);
    }

    void flushPendingReleases(GJBaseGameLayer* layer) {
        for (int i = 0; i < 2; i++) {
            if (s_button[i].pendingRelease) sendBufferedRelease(layer, i);
        }
    }

    // Ship and wave depend on exactly how long you hold.
    bool isHoldMode(PlayerObject* player) {
        return player && (player->m_isShip || player->m_isDart);
    }

    // Modes where letting go can steer you away from a hazard.
    bool canAutoRelease(PlayerObject* player) {
        return player && (player->m_isShip || player->m_isDart || player->m_isRobot);
    }

    // Which button a player listens to. In normal and dual mode both icons
    // follow player 1's clicks; in 2-player mode the second icon has its own.
    int buttonIndexFor(GJBaseGameLayer* layer, PlayerObject* player) {
        if (!layer) return -1;
        if (player == layer->m_player1) return 0;
        if (player == layer->m_player2) return isTwoPlayer(layer) ? 1 : 0;
        return -1;
    }

    void markConsumed(PlayerObject* player) {
        int index = buttonIndexFor(PlayLayer::get(), player);
        if (index < 0) return;
        auto& b = s_button[index];
        // A click we were holding for you just got used: that's a save.
        if (b.pendingRelease && !b.consumed) {
            s_bufferSaves++;
            s_labelDirty = true;
        }
        b.consumed = true;
    }

    // Is the player's hitbox still overlapping anything we forgave?
    GameObject* stillTouching(PlayerObject* player, GraceState const& g) {
        if (!player) return nullptr;
        auto const& playerRect = player->getObjectRect();
        for (auto hazard : g.hazards) {
            if (hazard && playerRect.intersectsRect(hazard->getObjectRect())) return hazard;
        }
        return nullptr;
    }
}

class $modify(LenientGameLayer, GJBaseGameLayer) {
    bool isPlaying() {
        return typeinfo_cast<PlayLayer*>(this) != nullptr;
    }

    // Should a release from this side be delayed by the input buffer?
    bool shouldBuffer(int index) {
        if (!boolSetting("input-buffer") || stepSetting("buffer-ticks") == 0) return false;
        if (boolSetting("buffer-ship-wave")) return true;
        if (index == 1) return !isHoldMode(m_player2);
        // Player 1's clicks drive both icons unless it's 2-player mode.
        if (isHoldMode(m_player1)) return false;
        if (!isTwoPlayer(this) && m_gameState.m_isDualMode && isHoldMode(m_player2)) return false;
        return true;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (s_injecting || button != static_cast<int>(PlayerButton::Jump) || !isPlaying()) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        int index = isPlayer1 ? 0 : 1;
        auto& b = s_button[index];

        if (down) {
            // A new click while an old one is still being held for you:
            // let go of the old one first so the game sees a fresh press.
            if (b.pendingRelease) sendBufferedRelease(this, index);
            b.held = true;
            b.consumed = false;
            b.autoReleased = false;
            b.autoReleaseQueued = false;
            return GJBaseGameLayer::handleButton(true, button, isPlayer1);
        }

        bool wasHeld = b.held;
        b.held = false;
        b.autoReleaseQueued = false;

        // Auto-release already let go for you, so the game doesn't need
        // to hear about this release.
        if (b.autoReleased) {
            b.autoReleased = false;
            return;
        }

        // Input buffer: the click hasn't done anything yet, so keep it held
        // for a few more steps in case you're about to land or reach an orb.
        if (wasHeld && !b.consumed && shouldBuffer(index)) {
            b.pendingRelease = true;
            b.releaseAt = s_step + stepSetting("buffer-ticks");
            return;
        }

        GJBaseGameLayer::handleButton(false, button, isPlayer1);
    }

    // Runs once per physics step.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!isPlaying()) return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        s_step++;

        // Input buffer: let go of held clicks that were used or ran out.
        // Auto-release: let go of clicks that were steering into a hazard.
        for (int i = 0; i < 2; i++) {
            auto& b = s_button[i];
            if (b.autoReleaseQueued) {
                b.autoReleaseQueued = false;
                if (b.pendingRelease) {
                    sendBufferedRelease(this, i);
                } else if (b.held && !b.autoReleased) {
                    inject(this, i, false);
                    b.autoReleased = true;
                }
            }
            if (b.pendingRelease && (b.consumed || s_step >= b.releaseAt)) {
                sendBufferedRelease(this, i);
            }
        }

        // Hazard grace: is each grace run still going, and has it run out?
        unsigned window = stepSetting("grace-ticks");
        PlayerObject* players[2] = { m_player1, m_player2 };
        for (int i = 0; i < 2; i++) {
            auto& g = s_grace[i];
            if (!g.active) continue;

            bool reportedLastStep = g.lastReport + 1 >= s_step;
            GameObject* touching = stillTouching(players[i], g);

            if (!reportedLastStep && !touching) {
                // Got clear of everything in time.
                s_graceSaves++;
                if (g.autoReleased) s_releaseSaves++;
                s_labelDirty = true;

                // If auto-release let go but you're still holding the real
                // button, give the hold back now that you're clear.
                int index = buttonIndexFor(this, players[i]);
                if (index >= 0 && s_button[index].autoReleased && s_button[index].held) {
                    s_button[index].autoReleased = false;
                    inject(this, index, true);
                }

                g = {};
                continue;
            }

            if (s_step - g.start >= window) {
                // Still inside a hazard after the whole window: die for real.
                GameObject* killer = touching ? touching : (g.hazards.empty() ? nullptr : g.hazards.back());
                g = {};
                if (players[i] && killer) {
                    s_forceKill = true;
                    static_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this))->destroyPlayer(players[i], killer);
                    s_forceKill = false;
                }
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// These spot the moment a click actually does something, so a buffered
// click is let go straight away and can't cause a second jump.
class $modify(LenientPlayer, PlayerObject) {
    void incrementJumps() {
        markConsumed(this);
        PlayerObject::incrementJumps();
    }

    void ringJump(RingObject* object, bool skipCheck) {
        // The game calls this to *check* an orb too, so only count it when
        // the orb actually changed the player's movement.
        auto velocityBefore = m_yVelocity;
        auto flippedBefore = m_isUpsideDown;
        auto positionBefore = m_position;

        PlayerObject::ringJump(object, skipCheck);

        if (m_yVelocity != velocityBefore || m_isUpsideDown != flippedBefore || !m_position.equals(positionBefore)) {
            markConsumed(this);
        }
    }

    void startDashing(DashRingObject* object) {
        markConsumed(this);
        PlayerObject::startDashing(object);
    }
};

class $modify(LenientPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* m_label = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        resetState();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto label = CCLabelBMFont::create("Leniency", "bigFont.fnt");
        label->setScale(0.3f);
        label->setOpacity(140);
        label->setAnchorPoint({0.f, 0.f});
        label->setPosition({4.f, 4.f});
        label->setID("leniency-indicator"_spr);
        this->addChild(label, 1000);
        m_fields->m_label = label;
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto label = m_fields->m_label;
        if (!label) return;

        // Settings can change from the pause menu, so check every frame.
        bool show = boolSetting("show-indicator");
        label->setVisible(show);
        if (!show || !s_labelDirty) return;
        s_labelDirty = false;

        if (boolSetting("show-saves")) {
            auto text = fmt::format("Leniency  grace {}  buffer {}", s_graceSaves, s_bufferSaves);
            if (boolSetting("auto-release")) text += fmt::format("  release {}", s_releaseSaves);
            label->setString(text.c_str());
        } else {
            label->setString("Leniency");
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // Our own "grace ran out" death, a death with no object (fell out of
        // the level etc.), and GD's anticheat spike all go straight through.
        // The anticheat spike is GD's built-in noclip check at the start of
        // a level and must never be forgiven.
        if (s_forceKill || !player || !object || object == m_anticheatSpike || !boolSetting("hazard-grace")) {
            return PlayLayer::destroyPlayer(player, object);
        }

        unsigned window = stepSetting("grace-ticks");
        if (window == 0) return PlayLayer::destroyPlayer(player, object);

        auto& g = s_grace[player == m_player2 ? 1 : 0];

        // No run going: this is a new touch, so its window starts now.
        if (!g.active) {
            g = {};
            g.active = true;
            g.start = s_step;

            // Auto-release: in ship, wave or robot, let go of the button
            // so you start moving away from the hazard.
            if (boolSetting("auto-release") && canAutoRelease(player)) {
                int index = buttonIndexFor(this, player);
                if (index >= 0) {
                    auto& b = s_button[index];
                    if ((b.held && !b.autoReleased) || b.pendingRelease) {
                        b.autoReleaseQueued = true;
                        g.autoReleased = true;
                    }
                }
            }
        }
        g.lastReport = s_step;
        if (std::find(g.hazards.begin(), g.hazards.end(), object) == g.hazards.end()) {
            g.hazards.push_back(object);
        }

        // Window used up: die now.
        if (s_step - g.start >= window) {
            g = {};
            return PlayLayer::destroyPlayer(player, object);
        }

        // Still inside the window: forgive this step.
    }

    void resetLevel() {
        flushPendingReleases(this);
        resetState();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        resetState();
        PlayLayer::onQuit();
    }
};

// A settings button in the pause menu, so you don't have to go back to
// the main menu to change anything.
class $modify(LenientPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        // A small blue "FL" circle, so it can't be mixed up with GD's own
        // options button.
        auto sprite = CircleButtonSprite::create(
            CCLabelBMFont::create("FL", "bigFont.fnt"), CircleBaseColor::Blue, CircleBaseSize::Small
        );
        auto button = CCMenuItemSpriteExtra::create(
            sprite, this, menu_selector(LenientPauseLayer::onLeniencySettings)
        );
        button->setID("settings-button"_spr);

        if (auto menu = this->getChildByID("right-button-menu")) {
            menu->addChild(button);
            menu->updateLayout();
        } else {
            // Fallback if the menu isn't there: our own menu in the top-left.
            auto winSize = CCDirector::get()->getWinSize();
            auto ownMenu = CCMenu::create();
            ownMenu->setID("settings-menu"_spr);
            ownMenu->setPosition({30.f, winSize.height - 30.f});
            ownMenu->addChild(button);
            this->addChild(ownMenu, 10);
        }
    }

    void onLeniencySettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }
};
