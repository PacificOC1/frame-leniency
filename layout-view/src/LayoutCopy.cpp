#include "LayoutCopy.hpp"

#include <charconv>
#include <unordered_map>
#include <vector>

namespace layout {
namespace {

// A GD "key,value,key,value" list, keeping the original order.
struct KeyValues {
    std::vector<std::pair<std::string, std::string>> items;

    static KeyValues parse(std::string const& text) {
        KeyValues kv;
        size_t start = 0;
        std::string key;
        bool haveKey = false;
        while (start <= text.size()) {
            size_t comma = text.find(',', start);
            if (comma == std::string::npos) comma = text.size();
            std::string part = text.substr(start, comma - start);
            if (!haveKey) {
                key = std::move(part);
                haveKey = true;
            } else {
                kv.items.emplace_back(std::move(key), std::move(part));
                haveKey = false;
            }
            start = comma + 1;
        }
        // A trailing key without a value is kept with an empty value, so
        // nothing in the original is silently lost.
        if (haveKey && !key.empty()) kv.items.emplace_back(std::move(key), std::string());
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

int toInt(std::string const* s, int fallback = 0) {
    if (!s || s->empty()) return fallback;
    int value = fallback;
    auto first = s->data();
    auto last = s->data() + s->size();
    // GD sometimes writes ints as floats ("1.0"); from_chars stops at the dot.
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc()) return fallback;
    return value;
}

bool isTrue(std::string const* s) {
    return s && (*s == "1" || *s == "true");
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
            return true;
        default:
            return false;
    }
}

} // namespace

bool isVisualTriggerID(int id) {
    if (id >= 2904 && id <= 2924) return true; // shader triggers
    switch (id) {
        case 899:  // colour
        case 1006: // pulse
        case 1520: // shake
        case 1818: // BG effect on
        case 1819: // BG effect off
        case 2903: // gradient
        case 3029: // BG change
        case 3030: // ground change
        case 3031: // middleground change
        case 3608: // spawn particle
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

    auto header = KeyValues::parse(levelString.substr(0, headerEnd));
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

    std::string out = header.join();
    out += ';';

    size_t start = headerEnd + 1;
    while (start < levelString.size()) {
        size_t end = levelString.find(';', start);
        if (end == std::string::npos) end = levelString.size();
        std::string_view raw(levelString.data() + start, end - start);
        start = end + 1;
        if (raw.empty()) continue;

        auto obj = KeyValues::parse(std::string(raw));
        int id = toInt(obj.get("1"), -1);
        if (id < 0) continue; // not an object
        ++st.total;

        ObjectInfo const& in = info(id);
        bool keep = true;
        if (alwaysKeep(id)) {
            keep = true;
        } else if (in.known && in.trigger) {
            if (opts.dropVisualTriggers && (in.colorTrigger || isVisualTriggerID(id))) {
                keep = false;
                ++st.droppedTriggers;
            }
        } else if (in.known && in.decoration) {
            keep = false;
            ++st.droppedDecoration;
        } else if (opts.dropNoTouch && (isTrue(obj.get("121")) || isTrue(obj.get("134")))) {
            keep = false;
            ++st.droppedNoTouch;
        } else if (!in.known && opts.dropVisualTriggers && isVisualTriggerID(id)) {
            keep = false;
            ++st.droppedTriggers;
        }
        if (!keep) continue;
        ++st.kept;

        if (!(in.known && in.trigger) && !alwaysKeep(id)) {
            obj.set("21", std::to_string(in.hazard ? kHazardChannel : kMainChannel));
            obj.set("22", std::to_string(kDetailChannel));
            obj.removeIf([](std::string const& k) {
                return k == "41" || k == "42" || k == "43" || k == "44"; // HSV
            });
            if (opts.dropGlow) obj.set("96", "1");
        }
        out += obj.join();
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
