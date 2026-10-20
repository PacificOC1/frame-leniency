#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace geode::prelude;

// Timing note: GD 2.2 runs physics at 240 steps per second, and
// m_gameState.m_currentProgress counts those steps. All windows below
// are measured in those steps ("physics frames").

namespace {
    // Hazard grace: tracks a run of consecutive frames in which the player
    // was touching something that would kill them.
    struct GraceState {
        bool active = false;
        unsigned start = 0;
        unsigned last = 0;
    };

    // Input buffer: a short tap is turned into a hold of at least N frames.
    struct BufferState {
        bool held = false;
        unsigned pressTick = 0;
        bool pendingRelease = false;
        unsigned releaseAt = 0;
    };

    GraceState s_grace[2];
    BufferState s_buffer[2];
    bool s_injecting = false;

    bool boolSetting(char const* key) {
        return Mod::get()->getSettingValue<bool>(key);
    }

    unsigned tickSetting(char const* key) {
        auto value = Mod::get()->getSettingValue<int64_t>(key);
        return value < 0 ? 0u : static_cast<unsigned>(value);
    }

    unsigned currentTick(GJBaseGameLayer* layer) {
        return layer->m_gameState.m_currentProgress;
    }

    void resetState() {
        for (auto& g : s_grace) g = {};
        for (auto& b : s_buffer) b = {};
    }

    // Sends a release straight to the game, skipping our own buffering.
    void sendRelease(GJBaseGameLayer* layer, int index) {
        s_buffer[index].pendingRelease = false;
        s_injecting = true;
        layer->handleButton(false, static_cast<int>(PlayerButton::Jump), index == 0);
        s_injecting = false;
    }

    void flushPendingReleases(GJBaseGameLayer* layer) {
        for (int i = 0; i < 2; i++) {
            if (s_buffer[i].pendingRelease) sendRelease(layer, i);
        }
    }
}

class $modify(LenientGameLayer, GJBaseGameLayer) {
    bool bufferActive() {
        return typeinfo_cast<PlayLayer*>(this) != nullptr
            && boolSetting("input-buffer")
            && tickSetting("buffer-ticks") > 0;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (s_injecting || button != static_cast<int>(PlayerButton::Jump) || !bufferActive()) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        int index = isPlayer1 ? 0 : 1;
        auto& b = s_buffer[index];
        unsigned now = currentTick(this);

        if (down) {
            // A new press while an old tap is still being held: let go of the
            // old one first so the game sees a fresh click.
            if (b.pendingRelease) sendRelease(this, index);
            b.held = true;
            b.pressTick = now;
            return GJBaseGameLayer::handleButton(true, button, isPlayer1);
        }

        unsigned minHold = tickSetting("buffer-ticks");
        bool wasHeld = b.held;
        b.held = false;

        // Tap was too short: keep it held and release it later.
        if (wasHeld && now >= b.pressTick && now < b.pressTick + minHold) {
            b.pendingRelease = true;
            b.releaseAt = b.pressTick + minHold;
            return;
        }

        GJBaseGameLayer::handleButton(false, button, isPlayer1);
    }

    // Runs once per physics step; releases any held taps whose time is up.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        unsigned now = currentTick(this);
        for (int i = 0; i < 2; i++) {
            auto& b = s_buffer[i];
            // "now < pressTick" covers the clock jumping back (checkpoint respawn).
            if (b.pendingRelease && (now >= b.releaseAt || now < b.pressTick)) {
                sendRelease(this, i);
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(LenientPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        resetState();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        if (boolSetting("show-indicator")) {
            auto label = CCLabelBMFont::create("Leniency", "bigFont.fnt");
            label->setScale(0.3f);
            label->setOpacity(140);
            label->setAnchorPoint({0.f, 0.f});
            label->setPosition({4.f, 4.f});
            label->setID("leniency-indicator"_spr);
            this->addChild(label, 1000);
        }
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // The anticheat spike is GD's built-in noclip check at the start of a
        // level; it must always go through or the game will flag the run.
        if (!player || !object || object == m_anticheatSpike || !boolSetting("hazard-grace")) {
            return PlayLayer::destroyPlayer(player, object);
        }

        unsigned window = tickSetting("grace-ticks");
        if (window == 0) return PlayLayer::destroyPlayer(player, object);

        auto& g = s_grace[player == m_player2 ? 1 : 0];
        unsigned now = currentTick(this);

        // More than one frame since the last deadly contact means this is a
        // new, separate touch, so its grace window starts fresh.
        if (!g.active || now < g.last || now > g.last + 1) {
            g.active = true;
            g.start = now;
        }
        g.last = now;

        // Still inside the grace window: forgive this frame.
        if (now - g.start < window) return;

        g.active = false;
        PlayLayer::destroyPlayer(player, object);
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
