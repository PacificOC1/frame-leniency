#include "../src/LayoutCopy.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using namespace layout;

static int fails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);     \
            ++fails;                                                     \
        }                                                                \
    } while (0)

static bool has(std::string const& s, std::string const& sub) { return s.find(sub) != std::string::npos; }

int main() {
    // Fake classification: 1 = block, 8 = spike, 1764 = deco, 899 = colour
    // trigger, 901 = move trigger, 1006 = pulse, 31 = start pos (typed deco),
    // 3032 keyframe (typed deco), 9999 unknown.
    Classifier cls = [](int id) {
        ObjectInfo i;
        i.known = id != 9999;
        switch (id) {
            case 1: break;
            case 8: i.hazard = true; break;
            case 1764: case 31: case 3032: i.decoration = true; break;
            case 899: i.trigger = true; i.colorTrigger = true; break;
            case 29: i.trigger = true; i.colorTrigger = true; break; // old BG trigger
            case 901: case 1006: i.trigger = true; break;
        }
        return i;
    };

    std::string level =
        "kS38,1_0_2_102_3_255_11_255_12_255_13_255_4_-1_6_1000_7_1_15_1_18_0_8_1|1_255_2_0_3_0_6_5|,"
        "kA13,0,kS1,10,kS29,abc,kA2,0;"
        "1,1,2,15,3,15,21,5,41,1,43,0a1a1a0a0;"   // block with custom colour + HSV
        "1,8,2,45,3,15;"                          // spike
        "1,1764,2,75,3,15;"                       // deco
        "1,899,2,80,3,100,23,5,7,255;"            // colour trigger
        "1,29,2,82,3,100;"                        // old BG colour trigger
        "1,901,2,85,3,100,51,3,28,30;"            // move trigger
        "1,1006,2,90,3,100;"                      // pulse
        "1,1,2,105,3,15,121,1;"                   // no-touch block
        "1,1,2,135,3,15,134,1;"                   // passable block
        "1,31,2,0,3,15,kA2,0;"                    // start pos (typed deco)
        "1,3032,2,3,3,3;"                         // keyframe (typed deco)
        "1,9999,2,1,3,1;"                         // unknown id: kept
        "1,2905,2,1,3,1;"                         // shader trigger, typed as trigger? (unknown to cls -> not trigger)
        ";";

    CopyStats st;
    CopyOptions opts;
    auto out = makeLayoutString(level, cls, opts, &st);
    std::printf("%s\n\n", out.c_str());

    CHECK(!out.empty());
    CHECK(st.total == 13);
    CHECK(st.droppedDecoration == 1);
    CHECK(st.droppedNoTouch == 2);
    CHECK(st.droppedTriggers == 3); // colour, old BG, pulse (2905 is "known" non-trigger here -> kept as object)
    CHECK(st.kept == st.total - 6);

    // Header: old colour keys gone, new kS38 present with our channels.
    CHECK(!has(out, "kS1,"));
    CHECK(!has(out, "kS29,"));
    CHECK(has(out, "kA13,0"));
    CHECK(has(out, "_6_1000_"));
    CHECK(has(out, "_6_998_"));
    CHECK(has(out, "1_40_2_40_3_40_11_255_12_255_13_255_4_-1_6_1000_"));
    CHECK(!has(out, "1_255_2_0_3_0_6_5"));

    // Kept objects.
    CHECK(has(out, "1,1,2,15,3,15,21,998,22,999,96,1;"));  // HSV removed, channel replaced
    CHECK(has(out, "1,8,2,45,3,15,21,997,22,999,96,1;"));  // hazard channel
    CHECK(has(out, "1,901,2,85,3,100,51,3,28,30;"));       // move trigger untouched
    CHECK(has(out, "1,31,2,0,3,15,kA2,0;"));               // start pos untouched
    CHECK(has(out, "1,3032,2,3,3,3;"));                    // keyframe untouched
    CHECK(has(out, "1,9999,"));                            // unknown kept

    // Dropped objects.
    CHECK(!has(out, "1,1764,"));
    CHECK(!has(out, "1,899,"));
    CHECK(!has(out, "1,29,"));
    CHECK(!has(out, "1,1006,"));
    CHECK(!has(out, "121,1"));
    CHECK(!has(out, "134,1"));

    // Unknown classifier: visual trigger IDs still dropped by ID.
    {
        Classifier none = [](int) { return ObjectInfo{}; };
        CopyStats s2;
        auto o2 = makeLayoutString("kA13,0;1,2905,2,1,3,1;1,1006,2,1,3,1;1,1,2,1,3,1;", none, opts, &s2);
        CHECK(!has(o2, "1,2905,"));
        CHECK(!has(o2, "1,1006,"));
        CHECK(has(o2, "1,1,2,1,3,1,21,998"));
        CHECK(s2.droppedTriggers == 2);
    }

    // Options off.
    {
        CopyOptions o;
        o.dropNoTouch = false;
        o.dropVisualTriggers = false;
        o.dropGlow = false;
        CopyStats s3;
        auto o3 = makeLayoutString(level, cls, o, &s3);
        CHECK(has(o3, "1,899,"));
        CHECK(has(o3, "121,1"));
        CHECK(!has(o3, "96,1"));
        CHECK(s3.droppedDecoration == 1 && s3.droppedTriggers == 0 && s3.droppedNoTouch == 0);
    }

    // Bad input.
    CHECK(makeLayoutString("", cls, opts).empty());
    CHECK(makeLayoutString("1,1,2,3;", cls, opts).empty());
    CHECK(makeLayoutString("no header at all", cls, opts).empty());

    // Names.
    CHECK(layoutLevelName("Stereo Madness") == "Stereo Madnes layout"); // 13 + 7 = 20
    CHECK(layoutLevelName("Bloodbath") == "Bloodbath layout");
    CHECK(layoutLevelName("Twelve chars").size() <= 20);
    CHECK(layoutLevelName("") == "Level layout");
    CHECK(layoutLevelName("ABCDEFGHIJKL M") == "ABCDEFGHIJKL layout"); // trailing space trimmed

    // Visual trigger IDs.
    CHECK(isVisualTriggerID(2904) && isVisualTriggerID(2924) && !isVisualTriggerID(2925));
    CHECK(!isVisualTriggerID(901) && !isVisualTriggerID(1007) && !isVisualTriggerID(1049));

    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
