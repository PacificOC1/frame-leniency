#include <Geode/Geode.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/ParticleGameObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/ShaderLayer.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/base64.hpp>

#include <unordered_map>

#include "LayoutCopy.hpp"

using namespace geode::prelude;

// Layout view: while it's on, the current level draws only its gameplay
// objects, in plain colours, over a plain background.
//
// How it works:
// - Decoration (GameObjectType::Decoration), and No Touch / Passable
//   objects if that setting is on, are kept in the level but never drawn:
//   GameObject::setVisible and setOpacity are clamped for them. They still
//   exist, so groups, keyframes and anything else that points at them keep
//   working exactly as normal.
// - Every colour GD gives a gameplay object goes through
//   GameObject::updateMainColor / updateSecondaryColor. We swap the colour
//   there, so colour triggers, pulses and HSV all end up plain.
// - Background, middleground, ground, gradients, glow, particles and shaders
//   are hidden or tinted every frame after GD's own visibility update.
// - Physics, hitboxes and triggers are never touched.

namespace lv {

struct Colours {
    ccColor3B main{255, 255, 255};
    ccColor3B detail{160, 160, 160};
    ccColor3B hazard{255, 255, 255};
    ccColor3B background{40, 40, 40};
    ccColor3B ground{25, 25, 25};
    ccColor3B line{255, 255, 255};
};

bool s_on = false;              // layout view is active in s_layer
bool s_inInit = false;          // s_layer is still inside PlayLayer::init
PlayLayer* s_layer = nullptr;   // the level we applied it to
Colours s_colours;
bool s_hideNoTouch = true;
bool s_showInvisible = true;
bool s_hideParticles = true;
bool s_skipShaders = true;
CCLayerColor* s_backdrop = nullptr; // plain background, a child of s_layer

ccColor3B colourSetting(char const* key) {
    return Mod::get()->getSettingValue<ccColor3B>(key);
}

void loadSettings() {
    auto mod = Mod::get();
    s_colours.main = colourSetting("object-colour");
    s_colours.detail = colourSetting("detail-colour");
    s_colours.hazard = colourSetting("hazard-colour");
    s_colours.background = colourSetting("background-colour");
    s_colours.ground = colourSetting("ground-colour");
    s_colours.line = colourSetting("line-colour");
    s_hideNoTouch = mod->getSettingValue<bool>("hide-no-touch");
    s_showInvisible = mod->getSettingValue<bool>("show-invisible");
    s_hideParticles = mod->getSettingValue<bool>("hide-particles");
    s_skipShaders = mod->getSettingValue<bool>("skip-shaders");
}

bool enabledSetting() {
    return Mod::get()->getSettingValue<bool>("enabled");
}

bool active() {
    return s_on && s_layer && (s_inInit || PlayLayer::get() == s_layer);
}

bool isPlayer(GameObject* obj) {
    return s_layer && (obj == s_layer->m_player1 || obj == s_layer->m_player2);
}

bool isHazard(GameObject* obj) {
    switch (obj->m_objectType) {
        case GameObjectType::Hazard:
        case GameObjectType::AnimatedHazard:
            return true;
        case GameObjectType::Slope:
            return obj->m_slopeIsHazard;
        default:
            return false;
    }
}

// Objects the layout view never draws.
bool isHidden(GameObject* obj) {
    if (obj->m_isTrigger) return false; // invisible in a real attempt anyway
    if (obj->m_objectType == GameObjectType::Decoration) return true;
    if (s_hideNoTouch && (obj->m_isNoTouch || obj->m_isPassable)) return true;
    return false;
}

// Whether our hooks should change this object right now.
bool handles(GameObject* obj) {
    return active() && !isPlayer(obj);
}

void tintTree(CCNode* node, ccColor3B colour) {
    if (!node) return;
    if (auto rgba = typeinfo_cast<CCRGBAProtocol*>(node)) rgba->setColor(colour);
    for (auto child : CCArrayExt<CCNode*>(node->getChildren())) tintTree(child, colour);
}

// Gradient layers are kept in a CCDictionary; we only need its values.
void setGradientsVisible(PlayLayer* pl, bool visible) {
    if (!pl->m_gradientLayers) return;
    for (auto element = pl->m_gradientLayers->m_pElements; element;
         element = static_cast<CCDictElement*>(element->hh.next)) {
        if (auto node = typeinfo_cast<CCNode*>(element->getObject())) node->setVisible(visible);
    }
}

void ensureBackdrop(PlayLayer* pl) {
    if (s_backdrop && s_backdrop->getParent() == pl) {
        s_backdrop->setColor(s_colours.background);
        s_backdrop->setVisible(true);
        return;
    }
    // Behind every other child of the play layer, and big enough to cover
    // the screen through camera zoom and rotation.
    int lowest = 0;
    for (auto child : CCArrayExt<CCNode*>(pl->getChildren())) lowest = std::min(lowest, child->getZOrder());
    auto win = CCDirector::get()->getWinSize();
    s_backdrop = CCLayerColor::create(ccc4(s_colours.background.r, s_colours.background.g, s_colours.background.b, 255),
                                      win.width * 4.f, win.height * 4.f);
    s_backdrop->setPosition({-win.width * 1.5f, -win.height * 1.5f});
    s_backdrop->setID("backdrop"_spr);
    pl->addChild(s_backdrop, lowest - 1);
}

// Runs every frame after GD's own visibility update, and once when the view
// is switched on.
void enforceScenery(PlayLayer* pl) {
    ensureBackdrop(pl);
    if (pl->m_background) pl->m_background->setVisible(false);
    if (pl->m_middleground) pl->m_middleground->setVisible(false);
    for (auto ground : {pl->m_groundLayer, pl->m_groundLayer2}) {
        if (!ground) continue;
        tintTree(ground->m_ground1Sprite, s_colours.ground);
        tintTree(ground->m_ground2Sprite, s_colours.ground);
        if (ground->m_lineSprite) ground->m_lineSprite->setColor(s_colours.line);
    }
    setGradientsVisible(pl, false);
}

void hideExtras(GameObject* obj, bool hidden) {
    if (obj->m_glowSprite) obj->m_glowSprite->setVisible(false);
    if (obj->m_particle && (hidden || s_hideParticles)) obj->m_particle->setVisible(false);
}

// Switches the view on or off in a running level.
void apply(PlayLayer* pl, bool on) {
    if (!pl) return;
    loadSettings();

    if (on) {
        s_layer = pl;
        s_on = true;
        enforceScenery(pl);
        for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (!obj || isPlayer(obj)) continue;
            bool hidden = isHidden(obj);
            if (hidden) {
                obj->setVisible(false);
            } else {
                obj->updateMainColor(isHazard(obj) ? s_colours.hazard : s_colours.main);
                obj->updateSecondaryColor(s_colours.detail);
            }
            hideExtras(obj, hidden);
        }
        return;
    }

    // Switching off mid-level is best effort: GD only pushes some colours
    // when they change, so we ask it to recolour every object, and show
    // what we hid. Re-opening the level always restores everything exactly.
    bool wasOn = s_on && s_layer == pl;
    s_on = false;
    if (!wasOn) return;
    if (s_backdrop) s_backdrop->setVisible(false);
    if (pl->m_background) pl->m_background->setVisible(true);
    if (pl->m_middleground) pl->m_middleground->setVisible(true);
    setGradientsVisible(pl, true);
    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (!obj || isPlayer(obj) || obj->m_isTrigger) continue;
        if (isHidden(obj) && !obj->m_isGroupDisabled && !obj->m_isHide) {
            obj->setVisible(true);
        }
        if (obj->m_glowSprite && !obj->m_hasNoGlow) obj->m_glowSprite->setVisible(true);
        if (obj->m_particle && !obj->m_hasNoParticles) obj->m_particle->setVisible(true);
        pl->manualUpdateObjectColors(obj);
    }
}

void toggle() {
    Mod::get()->setSettingValue<bool>("enabled", !enabledSetting());
    // The settings listener below does the actual switching.
}

} // namespace lv

// ---------------------------------------------------------------------------
// Objects

class $modify(LVGameObject, GameObject) {
    void setVisible(bool visible) {
        if (visible && lv::handles(this) && lv::isHidden(this)) visible = false;
        GameObject::setVisible(visible);
    }

    void setOpacity(unsigned char opacity) {
        if (!lv::handles(this)) return GameObject::setOpacity(opacity);
        bool hidden = lv::isHidden(this);
        if (hidden) opacity = 0;
        else if (lv::s_showInvisible && !m_isTrigger) opacity = 255;
        GameObject::setOpacity(opacity);
        lv::hideExtras(this, hidden);
    }

    void updateMainColor(ccColor3B const& colour) {
        if (!lv::handles(this) || lv::isHidden(this)) return GameObject::updateMainColor(colour);
        GameObject::updateMainColor(lv::isHazard(this) ? lv::s_colours.hazard : lv::s_colours.main);
    }

    void updateSecondaryColor(ccColor3B const& colour) {
        if (!lv::handles(this) || lv::isHidden(this)) return GameObject::updateSecondaryColor(colour);
        GameObject::updateSecondaryColor(lv::s_colours.detail);
    }

    void claimParticle() {
        GameObject::claimParticle();
        if (lv::handles(this)) lv::hideExtras(this, lv::isHidden(this));
    }
};

class $modify(LVParticleGameObject, ParticleGameObject) {
    // Particle objects are decoration, but their emitter isn't the object's
    // own sprite, so it needs hiding separately.
    void claimParticle() {
        ParticleGameObject::claimParticle();
        if (lv::handles(this) && m_particle) m_particle->setVisible(false);
    }
};

// ---------------------------------------------------------------------------
// The level

class $modify(LVPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        lv::loadSettings();
        lv::s_layer = this;
        lv::s_on = lv::enabledSetting();
        lv::s_backdrop = nullptr;
        lv::s_inInit = true;
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            lv::s_inInit = false;
            lv::s_layer = nullptr;
            lv::s_on = false;
            return false;
        }
        lv::s_inInit = false;
        if (lv::s_on) lv::apply(this, true);
        return true;
    }

    void updateVisibility(float dt) {
        PlayLayer::updateVisibility(dt);
        if (lv::s_on && lv::s_layer == this) lv::enforceScenery(this);
    }

    void toggleGlitter(bool visible) {
        if (lv::s_on && lv::s_layer == this) visible = false;
        PlayLayer::toggleGlitter(visible);
    }

    void onQuit() {
        if (lv::s_layer == this) {
            lv::s_on = false;
            lv::s_layer = nullptr;
            lv::s_backdrop = nullptr;
        }
        PlayLayer::onQuit();
    }
};

class $modify(LVShaderLayer, ShaderLayer) {
    // Draws the level without the shader pass. The render-texture sprite is
    // hidden for that frame in case it's a child, so a stale shader frame
    // can't be drawn over the level. Only done when the level is one of our
    // own children: otherwise drawing them directly wouldn't draw the level,
    // so the shaders are left on rather than risk a black screen.
    void visit() {
        if (!lv::s_skipShaders || !lv::s_on || !lv::s_layer || m_gameLayer != lv::s_layer ||
            m_gameLayer->getParent() != this) {
            return ShaderLayer::visit();
        }
        bool spriteVisible = m_sprite && m_sprite->isVisible();
        if (m_sprite) m_sprite->setVisible(false);
        CCNode::visit();
        if (m_sprite) m_sprite->setVisible(spriteVisible);
    }
};

// ---------------------------------------------------------------------------
// Layout copy

namespace lv {

layout::Rgb rgb(ccColor3B c) {
    return {c.r, c.g, c.b};
}

layout::ObjectInfo infoFor(GameObject* obj) {
    layout::ObjectInfo info;
    info.known = true;
    info.decoration = obj->m_objectType == GameObjectType::Decoration;
    info.trigger = obj->m_isTrigger;
    info.colorTrigger = obj->m_isColorTrigger;
    info.hazard = isHazard(obj);
    info.solid = obj->m_objectType == GameObjectType::Solid || obj->m_objectType == GameObjectType::Slope;
    return info;
}

// Your own level, uploaded and downloaded again (so it's no longer an
// editor level).
bool isOwnLevel(GJGameLevel* level) {
    int account = GJAccountManager::get()->m_accountID;
    if (account > 0 && level->m_accountID.value() == account) return true;
    int user = GameManager::get()->m_playerUserID.value();
    return user > 0 && level->m_userID.value() == user;
}

bool copyAllowed(GJGameLevel* level, std::string& why) {
    switch (level->m_levelType) {
        case GJLevelType::Editor:
        case GJLevelType::Main:
            return true;
        default:
            break;
    }
    if (level->m_password.value() == 1) return true; // free to copy
    if (isOwnLevel(level)) return true;
    why = "This level isn't free to copy, so the mod won't save a layout copy of it. "
          "The layout view itself still works.";
    return false;
}

// Returns a message for the popup.
std::string saveLayoutCopy(PlayLayer* pl) {
    if (!pl || !pl->m_level) return "No level is open.";
    auto level = pl->m_level;

    std::string why;
    if (!copyAllowed(level, why)) return why;

    std::string data = level->m_levelString;
    if (data.empty() && level->m_levelType == GJLevelType::Main) {
        if (auto full = LevelTools::getLevel(level->m_levelID.value(), false)) data = full->m_levelString;
    }
    if (data.empty()) return "Couldn't read this level's data.";

    std::string raw;
    if (data.starts_with("kS") || data.starts_with("kA")) {
        raw = data;
    } else {
        raw = ZipUtils::decompressString(data, false, 0);
    }
    if (raw.empty()) return "Couldn't unpack this level's data.";

    // Classify every object ID the way GD does, using the objects this level
    // already created. IDs that aren't loaded here (low detail mode skips
    // some) get a throwaway object to ask.
    std::unordered_map<int, layout::ObjectInfo> known;
    for (auto obj : CCArrayExt<GameObject*>(pl->m_objects)) {
        if (obj && !known.contains(obj->m_objectID)) known.emplace(obj->m_objectID, infoFor(obj));
    }
    layout::Classifier classify = [&](int id) -> layout::ObjectInfo {
        if (auto it = known.find(id); it != known.end()) return it->second;
        if (auto obj = GameObject::createWithKey(id)) return infoFor(obj);
        return {};
    };

    auto mod = Mod::get();
    layout::CopyOptions opts;
    opts.dropNoTouch = mod->getSettingValue<bool>("hide-no-touch");
    opts.dropVisualTriggers = mod->getSettingValue<bool>("copy-drop-visual-triggers");
    opts.dropAlphaTriggers = mod->getSettingValue<bool>("copy-drop-alpha-triggers");
    opts.dropUnusedTriggers = mod->getSettingValue<bool>("copy-drop-unused-triggers");
    opts.dropDuplicates = mod->getSettingValue<bool>("copy-drop-duplicates");
    opts.dropGlow = true;
    opts.background = rgb(s_colours.background);
    opts.ground = rgb(s_colours.ground);
    opts.line = rgb(s_colours.line);
    opts.main = rgb(s_colours.main);
    opts.detail = rgb(s_colours.detail);
    opts.hazard = rgb(s_colours.hazard);

    layout::CopyStats stats;
    auto result = layout::makeLayoutString(raw, classify, opts, &stats);
    if (result.empty()) return "Couldn't read this level's data.";

    auto copy = GameLevelManager::sharedState()->createNewLevel();
    if (!copy) return "GD couldn't create a new level.";
    copy->m_levelName = layout::layoutLevelName(level->m_levelName);
    copy->m_levelString = ZipUtils::compressString(result, false, 0);
    copy->m_audioTrack = level->m_audioTrack;
    copy->m_songID = level->m_songID;
    copy->m_songIDs = level->m_songIDs;
    copy->m_sfxIDs = level->m_sfxIDs;
    copy->m_twoPlayerMode = level->m_twoPlayerMode;
    std::string credit = "Layout of " + std::string(level->m_levelName);
    if (!std::string(level->m_creatorName).empty()) credit += " by " + std::string(level->m_creatorName);
    copy->m_levelDesc = utils::base64::encode(credit, utils::base64::Base64Variant::UrlWithPad);
    LocalLevelManager::sharedState()->save();

    auto message = fmt::format(
        "Saved <cy>{}</c> to your created levels.\n"
        "Kept <cg>{}</c> of {} objects. Left out {} decoration, {} no-touch/passable, "
        "{} visual triggers, {} unused triggers and {} stacked duplicates.",
        std::string(copy->m_levelName), stats.kept, stats.total, stats.droppedDecoration,
        stats.droppedNoTouch, stats.droppedTriggers, stats.droppedUnused, stats.droppedDuplicates
    );
    if (stats.anchors > 0) {
        message += fmt::format(
            "\n{} decoration/no-touch objects stay, hidden, because triggers use their position.",
            stats.anchors
        );
    }
    return message;
}

} // namespace lv

// ---------------------------------------------------------------------------
// Pause menu

class LayoutPopup : public geode::Popup {
protected:
    CCMenuItemToggler* m_toggle = nullptr;

    bool init() {
        if (!Popup::init(280.f, 190.f)) return false;
        this->setTitle("Layout View");

        auto size = m_mainLayer->getContentSize();
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        menu->setContentSize(size);
        m_mainLayer->addChild(menu);

        m_toggle = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(LayoutPopup::onToggle), 0.8f);
        m_toggle->toggle(lv::enabledSetting());
        m_toggle->setPosition({size.width / 2 - 70.f, size.height - 62.f});
        menu->addChild(m_toggle);

        auto label = CCLabelBMFont::create("Layout view", "bigFont.fnt");
        label->setScale(0.55f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition({size.width / 2 - 50.f, size.height - 62.f});
        m_mainLayer->addChild(label);

        auto copySprite = ButtonSprite::create("Save layout copy", "goldFont.fnt", "GJ_button_01.png", 0.8f);
        copySprite->setScale(0.85f);
        auto copyButton = CCMenuItemSpriteExtra::create(copySprite, this, menu_selector(LayoutPopup::onCopy));
        copyButton->setPosition({size.width / 2, size.height - 108.f});
        menu->addChild(copyButton);

        auto hint = CCLabelBMFont::create("Saves a new level with only the gameplay objects.", "chatFont.fnt");
        hint->setScale(0.6f);
        hint->setPosition({size.width / 2, size.height - 135.f});
        m_mainLayer->addChild(hint);

        auto settingsSprite = ButtonSprite::create("Settings", "goldFont.fnt", "GJ_button_04.png", 0.8f);
        settingsSprite->setScale(0.7f);
        auto settingsButton = CCMenuItemSpriteExtra::create(settingsSprite, this, menu_selector(LayoutPopup::onSettings));
        settingsButton->setPosition({size.width / 2, 28.f});
        menu->addChild(settingsButton);
        return true;
    }

    void onToggle(CCObject*) {
        // The toggler flips after this callback, so the new state is the
        // opposite of what it reports now.
        bool on = !m_toggle->isToggled();
        Mod::get()->setSettingValue<bool>("enabled", on);
    }

    void onCopy(CCObject*) {
        auto message = lv::saveLayoutCopy(PlayLayer::get());
        FLAlertLayer::create("Layout copy", message, "OK")->show();
    }

    void onSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }

public:
    static LayoutPopup* create() {
        auto ret = new LayoutPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(LVPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto sprite = CircleButtonSprite::create(
            CCLabelBMFont::create("LV", "bigFont.fnt"), CircleBaseColor::Gray, CircleBaseSize::Small
        );
        auto button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(LVPauseLayer::onLayoutView));
        button->setID("layout-view-button"_spr);

        if (auto menu = this->getChildByID("right-button-menu")) {
            menu->addChild(button);
            menu->updateLayout();
        } else {
            auto fallback = CCMenu::create();
            fallback->setID("layout-view-menu"_spr);
            auto win = CCDirector::get()->getWinSize();
            fallback->setPosition({win.width - 30.f, win.height / 2});
            fallback->addChild(button);
            this->addChild(fallback);
        }
    }

    void onLayoutView(CCObject*) {
        if (auto popup = LayoutPopup::create()) popup->show();
    }
};

// ---------------------------------------------------------------------------
// Settings and keybind

$on_mod(Loaded) {
    lv::loadSettings();

    listenForSettingChanges<bool>("enabled", [](bool on) {
        if (auto pl = PlayLayer::get()) lv::apply(pl, on);
    });

    // Colour and visibility settings changed from the pause menu apply
    // straight away.
    listenForAllSettingChanges([](std::string_view key, std::shared_ptr<SettingV3>) {
        if (key == "enabled") return;
        lv::loadSettings();
        if (auto pl = PlayLayer::get(); pl && lv::s_on && lv::s_layer == pl) lv::apply(pl, true);
    });

    listenForKeybindSettingPresses("toggle-key", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat || !PlayLayer::get()) return false;
        lv::toggle();
        return true;
    });
}
