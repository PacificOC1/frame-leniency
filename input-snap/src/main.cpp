// Input Snap - TEST BUILD
//
// Goal of this build: prove that the game can be rewound a few physics
// frames mid-attempt, cleanly and cheaply. When you would die, the mod
// instead loads a snapshot from N frames ago and lets you keep playing.
// The real auto-correct (re-running those frames with your input shifted)
// gets built on top of this once rewinding is known to work.
//
// Snapshots use the game's own practice-mode checkpoint system
// (PlayLayer::createCheckpoint / loadFromCheckpoint), which saves the
// players, triggers, timers and the rest of the game state.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>

#include <chrono>
#include <deque>
#include <vector>

using namespace geode::prelude;

namespace {
    struct Snapshot {
        Ref<CheckpointObject> checkpoint;
        unsigned step = 0;
    };

    // An orb, pad etc. the player activated, so it can be made usable
    // again if we rewind to before it was hit.
    struct Activation {
        EnhancedGameObject* object = nullptr;
        unsigned step = 0;
    };

    // Physics step counter: +1 at the start of every 240 TPS step.
    unsigned s_step = 0;

    std::deque<Snapshot> s_snapshots;
    std::vector<Activation> s_activations;

    // A death we turned into a rewind, handled at the start of the next step.
    bool s_rewindQueued = false;
    PlayerObject* s_pendingPlayer = nullptr;
    GameObject* s_pendingObject = nullptr;
    bool s_forceKill = false;

    // Stops endless rewinding if you keep dying in the same spot.
    unsigned s_lastRewindAt = 0;
    int s_rewindsInARow = 0;

    // Numbers for the overlay.
    int s_rewinds = 0;
    int s_failedRewinds = 0;
    double s_snapCostTotalUs = 0;
    double s_snapCostMaxUs = 0;
    int s_snapCount = 0;
    unsigned s_lastRewindDistance = 0;
    int s_lastReactivated = 0;

    bool boolSetting(char const* key) {
        return Mod::get()->getSettingValue<bool>(key);
    }

    unsigned intSetting(char const* key) {
        auto value = Mod::get()->getSettingValue<int64_t>(key);
        return value < 0 ? 0u : static_cast<unsigned>(value);
    }

    void resetAll() {
        s_step = 0;
        s_snapshots.clear();
        s_activations.clear();
        s_rewindQueued = false;
        s_pendingPlayer = nullptr;
        s_pendingObject = nullptr;
        s_lastRewindAt = 0;
        s_rewindsInARow = 0;
        s_rewinds = 0;
        s_failedRewinds = 0;
        s_snapCostTotalUs = 0;
        s_snapCostMaxUs = 0;
        s_snapCount = 0;
        s_lastRewindDistance = 0;
        s_lastReactivated = 0;
    }

    // How many snapshots to keep so a full rewind is always possible.
    size_t snapshotCapacity() {
        unsigned interval = std::max(1u, intSetting("snapshot-interval"));
        return intSetting("rewind-frames") / interval + 3;
    }

    void takeSnapshot(PlayLayer* pl) {
        auto start = std::chrono::steady_clock::now();
        auto checkpoint = pl->createCheckpoint();
        auto end = std::chrono::steady_clock::now();
        if (!checkpoint) return;

        double us = std::chrono::duration<double, std::micro>(end - start).count();
        s_snapCostTotalUs += us;
        s_snapCostMaxUs = std::max(s_snapCostMaxUs, us);
        s_snapCount++;

        s_snapshots.push_back({ checkpoint, s_step });
        while (s_snapshots.size() > snapshotCapacity()) s_snapshots.pop_front();

        // Forget activations older than anything we could rewind to.
        unsigned oldest = s_snapshots.front().step;
        std::erase_if(s_activations, [&](Activation const& a) { return a.step < oldest; });
    }

    // Returns true if the rewind happened.
    bool doRewind(PlayLayer* pl) {
        unsigned target = s_step > intSetting("rewind-frames") ? s_step - intSetting("rewind-frames") : 0;

        // Newest snapshot at or before the target step.
        Snapshot const* chosen = nullptr;
        for (auto it = s_snapshots.rbegin(); it != s_snapshots.rend(); ++it) {
            if (it->step <= target) { chosen = &*it; break; }
        }
        // Not far enough back yet (e.g. right after a restart): use the oldest.
        if (!chosen && !s_snapshots.empty()) chosen = &s_snapshots.front();
        if (!chosen) return false;

        unsigned to = chosen->step;
        Ref<CheckpointObject> checkpoint = chosen->checkpoint;

        pl->loadFromCheckpoint(checkpoint);

        // Orbs and pads used after that point can be used again.
        int reactivated = 0;
        std::erase_if(s_activations, [&](Activation const& a) {
            if (a.step < to) return false;
            if (a.object) {
                a.object->m_activatedByPlayer1 = false;
                a.object->m_activatedByPlayer2 = false;
                reactivated++;
            }
            return true;
        });

        s_lastRewindDistance = s_step - to;
        s_lastReactivated = reactivated;
        s_step = to;
        while (!s_snapshots.empty() && s_snapshots.back().step > to) s_snapshots.pop_back();
        return true;
    }
}

class $modify(SnapGameLayer, GJBaseGameLayer) {
    // Runs once per physics step.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = typeinfo_cast<PlayLayer*>(static_cast<GJBaseGameLayer*>(this));
        if (!pl) return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        s_step++;

        if (s_rewindQueued) {
            s_rewindQueued = false;
            if (doRewind(pl)) {
                s_rewinds++;
            } else {
                // Nothing to rewind to: die for real.
                s_failedRewinds++;
                if (s_pendingPlayer && s_pendingObject) {
                    s_forceKill = true;
                    pl->destroyPlayer(s_pendingPlayer, s_pendingObject);
                    s_forceKill = false;
                }
            }
            s_pendingPlayer = nullptr;
            s_pendingObject = nullptr;
        } else if (boolSetting("enabled")) {
            unsigned interval = std::max(1u, intSetting("snapshot-interval"));
            if (s_step % interval == 0) takeSnapshot(pl);
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(SnapObject, EnhancedGameObject) {
    void activatedByPlayer(PlayerObject* player) {
        EnhancedGameObject::activatedByPlayer(player);
        if (PlayLayer::get()) s_activations.push_back({ this, s_step });
    }
};

class $modify(SnapPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* m_overlay = nullptr;
        int m_frame = 0;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        resetAll();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto label = CCLabelBMFont::create("", "chatFont.fnt");
        label->setScale(0.5f);
        label->setOpacity(200);
        label->setAnchorPoint({0.f, 0.f});
        label->setAlignment(kCCTextAlignmentLeft);
        label->setPosition({4.f, 4.f});
        label->setID("overlay"_spr);
        this->addChild(label, 1000);
        m_fields->m_overlay = label;
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto label = m_fields->m_overlay;
        if (!label) return;

        bool show = boolSetting("show-overlay");
        label->setVisible(show);
        // Updating text every frame costs a little; 10 times a second is plenty.
        if (!show || (m_fields->m_frame++ % 6) != 0) return;

        double avg = s_snapCount ? s_snapCostTotalUs / s_snapCount : 0;
        label->setString(fmt::format(
            "Input Snap TEST  {}\n"
            "snapshot cost: avg {:.0f} us, max {:.0f} us ({} taken)\n"
            "rewinds: {}  failed: {}  last: {} frames back, {} orbs/pads reset\n"
            "step: {}",
            boolSetting("enabled") ? "ON" : "OFF",
            avg, s_snapCostMaxUs, s_snapCount,
            s_rewinds, s_failedRewinds, s_lastRewindDistance, s_lastReactivated,
            s_step
        ).c_str());
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // Straight through: our own real death, deaths with no object
        // (falling out of the level), GD's anticheat spike, or mod off.
        if (s_forceKill || !player || !object || object == m_anticheatSpike || !boolSetting("enabled")) {
            return PlayLayer::destroyPlayer(player, object);
        }

        // Already turning a death into a rewind this step.
        if (s_rewindQueued) return;

        // Dying again right after a rewind counts as "in a row".
        unsigned window = intSetting("rewind-frames") * 3;
        if (s_rewinds > 0 && s_step - s_lastRewindAt <= window) {
            s_rewindsInARow++;
        } else {
            s_rewindsInARow = 1;
        }
        s_lastRewindAt = s_step;

        if (s_rewindsInARow > static_cast<int>(intSetting("max-in-a-row"))) {
            s_rewindsInARow = 0;
            return PlayLayer::destroyPlayer(player, object);
        }

        // Don't die: rewind at the start of the next step instead.
        s_rewindQueued = true;
        s_pendingPlayer = player;
        s_pendingObject = object;
    }

    void resetLevel() {
        resetAll();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        resetAll();
        PlayLayer::onQuit();
    }
};
