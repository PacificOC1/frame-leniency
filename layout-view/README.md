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
- every gameplay object and every gameplay trigger (move, toggle, spawn, alpha, collision, item and so on),
- no decoration, and no No Touch or Passable objects if that setting is on,
- no colour, pulse, shake, gradient, shader, background/ground/middleground change or particle triggers (*Leave out visual triggers*),
- every object on three plain colour channels (997 hazards, 998 objects, 999 details) using your colour settings, with glow off,
- the same song, speed, gamemode and other level settings, and "Layout of *name* by *creator*" as its description.

Copies are only made of your own levels, main levels and levels that are **free to copy**. For anything else the mod says so and doesn't save a copy; the layout view still works on them.

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

## How it works

- `GameObject::setVisible` and `setOpacity` are clamped for decoration, so GD can't show it again.
- `GameObject::updateMainColor` and `updateSecondaryColor` get the plain colour instead of the channel colour. That's where GD applies channel colours, pulses and HSV, so they all end up plain.
- After `PlayLayer::updateVisibility` each frame, the background and middleground are hidden, the ground is tinted, gradients are hidden and a flat backdrop sits behind everything.
- `ShaderLayer::visit` draws its children directly instead of through the shader.
- The copy works on the level string (`LayoutCopy.cpp`, no Geode code, unit-tested in `test/`). Objects are classified using the level's own loaded objects (`m_objectType`, `m_isTrigger`, `m_isColorTrigger`), so the rules follow GD's own types. Start positions, keyframes, checkpoints and collision blocks are always kept.

## Known limits

- **Untested in-game.** It compiles cleanly against the 2.2081 Windows headers and the copy logic passes its unit tests, but it hasn't been run in GD yet.
- **Level-specific visuals.** A few effects aren't covered: screen shake, the orb/pad ring effects, the player's own trail and particles, and the level-start effects.
- **Changing *Hide No Touch* mid-level** won't re-show objects it already hid; re-open the level.
- **Visual trigger IDs in copies.** Colour triggers are found by GD's own flag; the others are matched by object ID (1006, 1520, 1818, 1819, 2903, 2904-2924, 3029-3031, 3608).
