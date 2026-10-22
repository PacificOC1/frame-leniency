#include "LayoutCopy.hpp"

#include <algorithm>
#include <charconv>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace layout {
namespace {

// Calls f(key, value) for every pair of a GD "key,value,key,value" list.
// A trailing key without a value is passed with an empty value.
template <class F>
void forEachPair(std::string_view text, F&& f) {
    size_t start = 0;
    while (start < text.size()) {
        size_t comma = text.find(',', start);
        if (comma == std::string_view::npos) comma = text.size();
        std::string_view key = text.substr(start, comma - start);
        std::string_view value;
        start = comma + 1;
        if (start <= text.size()) {
            size_t next = text.find(',', start);
            if (next == std::string_view::npos) next = text.size();
            value = text.substr(start, next - start);
            start = next + 1;
        }
        if (!key.empty()) f(key, value);
    }
}

// A GD "key,value,key,value" list, keeping the original order.
struct KeyValues {
    std::vector<std::pair<std::string, std::string>> items;

    static KeyValues parse(std::string_view text) {
        KeyValues kv;
        forEachPair(text, [&](std::string_view k, std::string_view v) { kv.items.emplace_back(k, v); });
        return kv;
    }

    std::string const* get(std::string_view key) const {
        for (auto& [k, v] : items) {
            if (k == key) return &v;
        }
        return nullptr;
    }

    void set(std::string_view key, std::string value) {
        for (auto& [k, v] : items) {
            if (k == key) {
                v = std::move(value);
                return;
            }
        }
        items.emplace_back(std::string(key), std::move(value));
    }

    template <class Pred>
    void removeIf(Pred pred) {
        std::erase_if(items, [&](auto const& kv) { return pred(kv.first); });
    }

    std::string join() const {
        std::string out;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i) out += ',';
            out += items[i].first;
            out += ',';
            out += items[i].second;
        }
        return out;
    }
};

int toInt(std::string_view s, int fallback = 0) {
    if (s.empty()) return fallback;
    int value = fallback;
    // GD sometimes writes ints as floats ("1.0"); from_chars stops at the dot.
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc()) return fallback;
    return value;
}

bool isTrue(std::string_view s) {
    return s == "1" || s == "true";
}

std::string channel(int id, Rgb c) {
    // The same layout GD writes for a plain channel: rgb, no player colour,
    // no blending, full opacity, no copy.
    return "1_" + std::to_string(c.r) + "_2_" + std::to_string(c.g) + "_3_" + std::to_string(c.b) +
        "_11_255_12_255_13_255_4_-1_6_" + std::to_string(id) + "_7_1_15_1_18_0_8_1";
}

std::string channelString(CopyOptions const& o) {
    std::string out;
    auto add = [&](int id, Rgb c) {
        out += channel(id, c);
        out += '|';
    };
    add(1000, o.background); // BG
    add(1001, o.ground);     // G1
    add(1009, o.ground);     // G2
    add(1013, o.background); // MG
    add(1014, o.background); // MG2
    add(1002, o.line);       // Line
    add(1004, o.main);       // Obj
    add(1003, o.main);       // 3DL
    add(kHazardChannel, o.hazard);
    add(kMainChannel, o.main);
    add(kDetailChannel, o.detail);
    return out;
}

// Old colour keys in the header (kS1-kS37). kS38 replaces them all.
bool isOldColourKey(std::string const& key) {
    if (key.size() < 3 || key[0] != 'k' || key[1] != 'S') return false;
    int n = 0;
    for (size_t i = 2; i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') return false;
        n = n * 10 + (key[i] - '0');
    }
    return n >= 1 && n <= 37;
}

// Objects that never draw anything in a real attempt but must stay for the
// level to play the same, whatever type GD gives them.
bool alwaysKeep(int id) {
    switch (id) {
        case 31:   // start position
        case 1816: // collision block
        case 2063: // checkpoint
        case 3032: // keyframe
        case 3640: // collision state block
        case 3643: // toggle block
            return true;
        default:
            return false;
    }
}

// Keys that hold a group whose *position* the object uses: a centre, a
// target, a destination. Objects in those groups must stay in the copy even
// if they're decoration, or the trigger moves things to the wrong place.
// From the 2.2 property list (gd-info-explorer's schema).
std::span<char const* const> positionalKeys(int id) {
    static constexpr char const* k71[] = {"71"};
    static constexpr char const* k51[] = {"51"};
    static constexpr char const* move[] = {"71", "395"};                     // target pos, centre
    static constexpr char const* rotate[] = {"71", "516", "517", "518", "519"}; // centre, aim min/max x/y
    static constexpr char const* sfx[] = {"51", "71", "457"};                 // sound source / listener groups
    if (id >= 3006 && id <= 3015) return k71; // area and edit area triggers: centre
    switch (id) {
        case 901: return move;
        case 1346: return rotate;
        case 2067: // scale: centre
        case 1347: // follow: follow group
        case 3033: // keyframe animation: parent group
        case 3016: // advanced follow: follow group
        case 3661: // retarget advanced follow: follow group
        case 1914: // static camera: target
        case 3600: // end: target position
        case 3613: // UI: UI target
        case 3608: // spawn particle: position group
            return k71;
        case 747:  // linked teleport portals
        case 2902: // unlinked teleport portal
        case 3022: // teleport trigger
        case 3027: // teleport orb
        case 2062: // camera edge
        case 2905: case 2907: case 2913: case 2914: case 2915: case 2916: // shader centres
            return k51;
        case 3603: // edit SFX
        case 3605: // edit song
            return sfx;
        default:
            return {};
    }
}

// Triggers whose only effect is on the objects in their target group (51).
// With nothing left in that group they do nothing, so the copy drops them.
bool actsOnlyOnTarget(int id) {
    switch (id) {
        case 901:  // move
        case 1007: // alpha
        case 1049: // toggle
        case 1268: // spawn
        case 1346: // rotate
        case 2067: // scale
        case 1347: // follow
        case 1585: // animate
        case 1814: // follow player Y
        case 3016: // advanced follow
        case 1595: // touch
        case 1611: // count
        case 1811: // instant count
        case 1815: // collision
        case 1812: // on death
        case 3604: // event
            return true;
        default:
            return false;
    }
}

// Calls f(group) for each group in a "1.2.3" list.
template <class F>
void forEachGroup(std::string_view list, F&& f) {
    size_t start = 0;
    while (start < list.size()) {
        size_t dot = list.find('.', start);
        if (dot == std::string_view::npos) dot = list.size();
        int g = toInt(list.substr(start, dot - start));
        if (g > 0) f(g);
        start = dot + 1;
    }
}

// Group IDs are stored as shorts in GD, so a flat table covers them all.
struct GroupSet {
    std::vector<uint8_t> bits = std::vector<uint8_t>(65536, 0);
    void clear() { std::fill(bits.begin(), bits.end(), 0); }
    void add(int g) {
        if (g > 0 && g < 65536) bits[g] = 1;
    }
    bool has(int g) const { return g > 0 && g < 65536 && bits[g]; }
};

enum class Kind : uint8_t {
    Object,    // gameplay object: kept, recoloured
    Trigger,   // gameplay trigger: kept as it is, unless unused
    Fixed,     // alwaysKeep: kept as it is
    Droppable, // decoration / No Touch: kept hidden only if a trigger needs it
    Visual,    // visual trigger: dropped
};

enum class Reason : uint8_t { None, Decoration, NoTouch, Unused, Duplicate };

struct Obj {
    std::string_view raw;
    int id = 0;
    int target = 0; // key 51
    uint32_t groupsBegin = 0, groupsCount = 0;
    uint32_t refsBegin = 0, refsCount = 0;
    Kind kind = Kind::Object;
    Reason reason = Reason::None; // why a Droppable or a dropped object goes
    bool keep = true;
    bool groupParent = false;
    bool dedupe = false; // block, spike or slope that can be deduplicated
};

// The object as it's written to the copy.
std::string transformed(Obj const& o, ObjectInfo const& in, CopyOptions const& opts) {
    auto obj = KeyValues::parse(o.raw);
    if (o.kind == Kind::Droppable) {
        // Kept only for its position: never drawn.
        obj.set("135", "1"); // hide
        obj.set("507", "1"); // no particles
        if (opts.dropGlow) obj.set("96", "1");
    } else if (o.kind == Kind::Object) {
        obj.set("21", std::to_string(in.hazard ? kHazardChannel : kMainChannel));
        obj.set("22", std::to_string(kDetailChannel));
        obj.removeIf([](std::string const& k) {
            return k == "41" || k == "42" || k == "43" || k == "44"; // HSV
        });
        if (opts.dropGlow) obj.set("96", "1");
    }
    return obj.join();
}

// The same object with keys that only change how it's drawn or edited left
// out, sorted, so stacked copies compare equal.
std::string dedupeKey(std::string const& written) {
    auto kv = KeyValues::parse(written);
    kv.removeIf([](std::string const& k) {
        return k == "20" || k == "61" || k == "24" || k == "25" || k == "155" || k == "156";
    });
    std::sort(kv.items.begin(), kv.items.end());
    return kv.join();
}

} // namespace

bool isVisualTriggerID(int id) {
    if (id >= 2904 && id <= 2924) return true; // shader triggers
    switch (id) {
        case 32:   // enable ghost trail
        case 33:   // disable ghost trail
        case 899:  // colour
        case 1006: // pulse
        case 1520: // shake
        case 1612: // hide player
        case 1613: // show player
        case 1818: // BG effect on
        case 1819: // BG effect off
        case 2016: // camera guide (editor only)
        case 2903: // gradient
        case 2999: // edit middleground
        case 3010: // area tint
        case 3015: // edit area tint
        case 3021: // enter tint
        case 3029: // BG change
        case 3030: // ground change
        case 3031: // middleground change
        case 3606: // BG speed
        case 3608: // spawn particle
        case 3612: // middleground speed
        case 3642: // BPM guide (editor only)
            return true;
        default:
            return false;
    }
}

bool isAlphaTriggerID(int id) {
    switch (id) {
        case 1007: // alpha
        case 3009: // area fade
        case 3014: // edit area fade
        case 3020: // enter fade
            return true;
        default:
            return false;
    }
}

std::string makeLayoutString(std::string const& levelString, Classifier const& classify,
                             CopyOptions const& opts, CopyStats* stats) {
    CopyStats local;
    CopyStats& st = stats ? *stats : local;
    st = {};

    size_t headerEnd = levelString.find(';');
    if (headerEnd == std::string::npos || headerEnd == 0) return {};

    auto header = KeyValues::parse(std::string_view(levelString).substr(0, headerEnd));
    if (header.items.empty() || header.items.front().first.empty() ||
        header.items.front().first[0] != 'k') {
        return {};
    }
    header.removeIf(isOldColourKey);
    header.set("kS38", channelString(opts));

    std::unordered_map<int, ObjectInfo> cache;
    auto info = [&](int id) -> ObjectInfo const& {
        auto it = cache.find(id);
        if (it == cache.end()) it = cache.emplace(id, classify ? classify(id) : ObjectInfo{}).first;
        return it->second;
    };
    auto isVisual = [&](int id, ObjectInfo const& in) {
        if (opts.dropVisualTriggers && (isVisualTriggerID(id) || (in.known && in.colorTrigger))) return true;
        return opts.dropAlphaTriggers && isAlphaTriggerID(id);
    };

    // Pass 1: read every object once, without copying it.
    std::vector<Obj> objs;
    std::vector<int> groupPool;
    std::vector<int> refPool;
    std::string_view all(levelString);
    size_t start = headerEnd + 1;
    while (start < all.size()) {
        size_t end = all.find(';', start);
        if (end == std::string_view::npos) end = all.size();
        std::string_view raw = all.substr(start, end - start);
        start = end + 1;
        if (raw.empty()) continue;

        Obj o;
        o.raw = raw;
        o.id = -1;
        bool noTouch = false;
        auto refKeys = std::span<char const* const>{};
        // The ID is almost always first, but don't rely on it.
        forEachPair(raw, [&](std::string_view k, std::string_view v) {
            if (k == "1") o.id = toInt(v, -1);
        });
        if (o.id < 0) continue; // not an object
        ++st.total;
        refKeys = positionalKeys(o.id);
        o.groupsBegin = static_cast<uint32_t>(groupPool.size());
        o.refsBegin = static_cast<uint32_t>(refPool.size());
        forEachPair(raw, [&](std::string_view k, std::string_view v) {
            if (k == "57") forEachGroup(v, [&](int g) { groupPool.push_back(g); });
            else if (k == "33") { if (int g = toInt(v); g > 0) groupPool.push_back(g); }
            else if (k == "51") o.target = toInt(v);
            else if (k == "121" || k == "134") noTouch = noTouch || isTrue(v);
            else if (k == "34" || k == "279") o.groupParent = o.groupParent || isTrue(v);
            for (auto rk : refKeys) {
                if (k == rk) {
                    if (int g = toInt(v); g > 0) refPool.push_back(g);
                }
            }
        });
        o.groupsCount = static_cast<uint32_t>(groupPool.size()) - o.groupsBegin;
        o.refsCount = static_cast<uint32_t>(refPool.size()) - o.refsBegin;

        ObjectInfo const& in = info(o.id);
        if (alwaysKeep(o.id)) {
            o.kind = Kind::Fixed;
        } else if (in.known && in.trigger) {
            o.kind = isVisual(o.id, in) ? Kind::Visual : Kind::Trigger;
        } else if (in.known && in.decoration) {
            o.kind = Kind::Droppable;
            o.reason = Reason::Decoration;
        } else if (opts.dropNoTouch && noTouch) {
            o.kind = Kind::Droppable;
            o.reason = Reason::NoTouch;
        } else if (!in.known && isVisual(o.id, in)) {
            o.kind = Kind::Visual;
        } else {
            o.kind = Kind::Object;
            o.dedupe = in.known && (in.solid || in.hazard);
        }
        o.keep = o.kind != Kind::Visual;
        objs.push_back(o);
    }

    auto groupsOf = [&](Obj const& o) {
        return std::span<int const>(groupPool.data() + o.groupsBegin, o.groupsCount);
    };
    auto refsOf = [&](Obj const& o) {
        return std::span<int const>(refPool.data() + o.refsBegin, o.refsCount);
    };

    // Pass 2: decide what stays. Dropping an unused trigger can empty another
    // group (a spawn trigger's, say) or free a decoration anchor, so repeat
    // until nothing changes. Stopping early is always safe: it only keeps
    // more than it needs to.
    GroupSet anchors;
    GroupSet members;
    constexpr int kMaxRounds = 64;
    for (int round = 0; round < kMaxRounds; ++round) {
        anchors.clear();
        for (auto const& o : objs) {
            if (o.keep && o.kind != Kind::Droppable) {
                for (int g : refsOf(o)) anchors.add(g);
            }
        }
        for (auto& o : objs) {
            if (o.kind != Kind::Droppable) continue;
            o.keep = o.groupParent ||
                std::ranges::any_of(groupsOf(o), [&](int g) { return anchors.has(g); });
        }
        if (!opts.dropUnusedTriggers) break;

        members.clear();
        for (auto const& o : objs) {
            if (o.keep) {
                for (int g : groupsOf(o)) members.add(g);
            }
        }
        bool changed = false;
        for (auto& o : objs) {
            if (!o.keep || o.kind == Kind::Fixed || o.kind == Kind::Droppable) continue;
            if (!actsOnlyOnTarget(o.id) || o.target <= 0 || members.has(o.target)) continue;
            o.keep = false;
            o.reason = Reason::Unused;
            changed = true;
        }
        if (!changed) break;
    }

    // Pass 3: write what stays, dropping stacked duplicates on the way.
    std::string out = header.join();
    out += ';';
    // Dedupe key hash -> the first object with it. Only hashes are stored, so
    // a level with half a million blocks doesn't keep a second copy of them;
    // a hash match is confirmed by comparing the real keys.
    std::unordered_map<size_t, size_t> seen;
    for (size_t i = 0; i < objs.size(); ++i) {
        auto& o = objs[i];
        if (!o.keep) {
            switch (o.reason) {
                case Reason::Decoration: ++st.droppedDecoration; break;
                case Reason::NoTouch: ++st.droppedNoTouch; break;
                case Reason::Unused: ++st.droppedUnused; break;
                default: ++st.droppedTriggers; break;
            }
            continue;
        }
        ObjectInfo const& in = info(o.id);
        std::string written = transformed(o, in, opts);
        if (opts.dropDuplicates && o.dedupe) {
            std::string key = dedupeKey(written);
            auto [it, fresh] = seen.emplace(std::hash<std::string>{}(key), i);
            if (!fresh) {
                auto const& first = objs[it->second];
                if (dedupeKey(transformed(first, info(first.id), opts)) == key) {
                    o.keep = false;
                    o.reason = Reason::Duplicate;
                    ++st.droppedDuplicates;
                    continue;
                }
            }
        }
        ++st.kept;
        if (o.kind == Kind::Droppable) ++st.anchors;
        out += written;
        out += ';';
    }
    return out;
}

std::string layoutLevelName(std::string const& original) {
    constexpr size_t kMax = 20;
    std::string const suffix = " layout";
    std::string base = original.empty() ? std::string("Level") : original;
    if (base.size() + suffix.size() > kMax) {
        base.resize(kMax - suffix.size());
        while (!base.empty() && base.back() == ' ') base.pop_back();
    }
    return base + suffix;
}

} // namespace layout
