# Layout View (Geode mod, GD 2.2081)

Plays any level as a bare layout, and can save a layout copy as a new level.

## Layout view

Turn it on from the **LV** button in the pause menu, or press **Ctrl+L** in a level (change the key in the mod's settings). It switches on and off straight away; you don't need to restart.

While it's on:
- **Only gameplay objects are drawn**: blocks, spikes, saws, slopes, orbs, pads, portals, coins and so on, with their normal textures.
- **Decoration isn't drawn.** It's still in the level, so moving groups, keyframes and toggles behave exactly as normal.
- **No Touch and Passable objects are hidden too**, since they can't affect you. Turn off *Hide No Touch and Passable objects* to see them.
- **Colours are plain.** Every object uses *Object colour* and *Detail colour*, and hazards use *Hazard colour*. Colour triggers, pulses and HSV have no effect. Set *Hazard colour* to red if you want hazards to stand out.
- **The background is one flat colour.** The middleground, gradients, glow and particles are hidden, and the ground is tinted *Ground colour* with a *Ground line colour* line.
- **Shader effects are skipped** (*Skip shader effects*).
- **Invisible gameplay objects are shown** at full opacity (*Show invisible gameplay objects*), so alpha tricks and fading blocks are visible.
- **Physics are untouched.** Hitboxes, triggers and timing are exactly the same as with it off.

Switching it **off** mid-level is best effort: some colours, like the ground's, only come back when the level changes them. Re-open the level for an exact restore.

## Layout copy

Pause, press **LV**, then **Save layout copy**. A new level called "*name* layout" appears in your created levels. It has:
- every gameplay object and every gameplay trigger that still does something (move, toggle, spawn, collision, item and so on),
- no decoration, and no No Touch or Passable objects if that setting is on, **except** ones a trigger needs the position of. Those stay, hidden (see below),
- no colour, pulse, shake, gradient, shader, background/ground/middleground change, particle, ghost trail or show/hide player triggers (*Leave out visual triggers*),
- no alpha or fade triggers (*Leave out alpha and fade triggers*): opacity never changes gameplay,
- no **unused triggers** (*Leave out unused triggers*): a move, spawn, toggle, rotate, scale, follow, animate, touch, count, collision, on-death or event trigger whose target group has nothing left in the copy. This repeats, so a spawn trigger that only spawned dropped triggers goes too,
- no **stacked duplicates** (*Merge stacked duplicates*): a block, spike or slope that's an exact copy of another one (same place, rotation, scale, groups and settings; colour, Z order and editor layer don't count) is kept once. Triggers are never merged,
- every object on three plain colour channels (997 hazards, 998 objects, 999 details) using your colour settings, with glow off,
- the same song, speed, gamemode and other level settings, and "Layout of *name* by *creator*" as its description.

**Objects kept for their position.** Levels often use an invisible decoration object as a rotate centre, a move or teleport target, a follow group or a camera target. Dropping it would change where things move, so the copy keeps any decoration or No Touch object that is in a group a kept trigger uses that way, or that is a group parent, with *Hide* turned on. The keys are taken from the 2.2 property list: 71 and 395 (move), 71 and 516-519 (rotate), 71 (scale, follow, keyframe animation, advanced follow, area, static camera, end, UI), 51 (teleport portals, orbs and triggers, camera edge). The popup says how many were kept this way.

Copies are only made of your own levels (including ones you uploaded and downloaded again), main levels and levels that are **free to copy**. For anything else the mod says so and doesn't save a copy; the layout view still works on them.

## Settings

| Setting | Default | What it does |
|---|---|---|
| Layout view | off | The view itself. Same as the LV toggle and the keybind. |
| Toggle keybind | Ctrl+L | Switches the view in a level. |
| Hide No Touch and Passable objects | on | Hides them in the view and leaves them out of copies. |
| Show invisible gameplay objects | on | Draws gameplay objects fully opaque. |
| Hide particles | on | Hides orb, pad and portal particles. Particle objects are always hidden. |
| Skip shader effects | on | Draws without shader trigger effects. Turn off if a shader level looks wrong. |
| Object / Detail / Hazard colour | white / grey / white | Object colours, in the view and in copies. |
| Background / Ground / Ground line colour | dark grey / darker grey / white | Scenery colours. |
| Leave out visual triggers | on | For copies only. |
| Leave out alpha and fade triggers | on | For copies only. |
| Leave out unused triggers | on | For copies only. |
| Merge stacked duplicates | on | For copies only. |

## How it works

- `GameObject::setVisible` and `setOpacity` are clamped for decoration, so GD can't show it again.
- `GameObject::updateMainColor` and `updateSecondaryColor` get the plain colour instead of the channel colour. That's where GD applies channel colours, pulses and HSV, so they all end up plain.
- After `PlayLayer::updateVisibility` each frame, the background and middleground are hidden, the ground is tinted, gradients are hidden and a flat backdrop sits behind everything.
- `ShaderLayer::visit` draws its children directly instead of through the shader, but only if the level is one of its children; otherwise shaders are left on.
- The copy works on the level string (`LayoutCopy.cpp`, no Geode code, unit-tested in `test/`). Objects are classified using the level's own loaded objects (`m_objectType`, `m_isTrigger`, `m_isColorTrigger`), so the rules follow GD's own types. Start positions, keyframes, checkpoints, collision blocks, collision state blocks and toggle blocks are always kept.
- The copy reads each object once without copying it, then decides what stays in rounds (dropping an unused trigger can empty another group or free a hidden anchor) until nothing changes, and writes the result. Duplicates are found by hash, confirmed by comparing the objects, so big levels don't need a second copy in memory.

## Known limits

- **Untested in-game.** It compiles cleanly against the 2.2081 Windows headers and the copy logic passes its unit tests, but it hasn't been run in GD yet.
- **Level-specific visuals.** A few effects aren't covered: screen shake, the orb/pad ring effects, the player's own trail and particles, and the level-start effects.
- **Changing *Hide No Touch* mid-level** won't re-show objects it already hid; re-open the level.
- **Visual trigger IDs in copies.** Colour triggers are found by GD's own flag; the others are matched by object ID (32, 33, 1006, 1520, 1612, 1613, 1818, 1819, 2016, 2903, 2904-2924, 2999, 3010, 3015, 3021, 3029-3031, 3606, 3608, 3612, 3642; alpha and fade: 1007, 3009, 3014, 3020).
- **Enter effect triggers are kept.** Old ones (22-28, 55-59, 1915) and the 2.2 enter move/rotate/scale ones can move objects, so they're not treated as visual.
- **Unused triggers are only checked through their target group (51).** Triggers that can target a control ID or an effect ID instead (stop, edit area, edit advanced follow) and ones with more than one target (random, sequence, item compare) are always kept.
