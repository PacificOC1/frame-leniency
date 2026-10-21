#pragma once

// Pure logic for the layout copy: takes a decompressed GD level string and
// returns a new one with only the gameplay objects, in plain colours.
// No Geode or cocos includes, so it can be unit-tested on any machine.

#include <cstdint>
#include <functional>
#include <string>

namespace layout {

struct Rgb {
    uint8_t r = 255, g = 255, b = 255;
};

// What the game says about one object ID. Filled in from the live level's
// objects, so every rule follows GD's own classification.
struct ObjectInfo {
    bool known = false;        // false: we couldn't classify it, so it's kept
    bool decoration = false;   // GameObjectType::Decoration
    bool trigger = false;      // m_isTrigger
    bool colorTrigger = false; // m_isColorTrigger (also the old 1.x/2.0 ones)
    bool hazard = false;       // spikes, saws, hazard slopes
};

using Classifier = std::function<ObjectInfo(int id)>;

struct CopyOptions {
    bool dropNoTouch = true;        // drop objects with No Touch (121) or Passable (134)
    bool dropVisualTriggers = true; // colour, pulse, shake, gradient, shader, BG/ground/MG change...
    bool dropGlow = true;           // sets No Glow (96) on what's kept
    Rgb background{40, 40, 40};
    Rgb ground{25, 25, 25};
    Rgb line{255, 255, 255};
    Rgb main{255, 255, 255};
    Rgb detail{160, 160, 160};
    Rgb hazard{255, 255, 255};
};

// Colour channels the copy assigns to every kept object.
constexpr int kHazardChannel = 997;
constexpr int kMainChannel = 998;
constexpr int kDetailChannel = 999;

struct CopyStats {
    int total = 0;          // objects in the original
    int kept = 0;
    int droppedDecoration = 0;
    int droppedNoTouch = 0;
    int droppedTriggers = 0;
};

// Purely visual triggers that aren't flagged as colour triggers.
bool isVisualTriggerID(int id);

// Builds the layout level string. `levelString` is the decompressed string
// ("kS38,...;1,1,2,15,3,15;..."). Returns an empty string if it has no header.
std::string makeLayoutString(std::string const& levelString, Classifier const& classify,
                             CopyOptions const& opts, CopyStats* stats = nullptr);

// "Stereo Madness" -> "Stereo Madness LV", cut to GD's 20 character limit.
std::string layoutLevelName(std::string const& original);

} // namespace layout
