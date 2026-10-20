# Frame Leniency (Geode mod)

Targets Geometry Dash 2.2081 and Geode SDK v5.11.2.

## Build

1. Install the Geode loader into GD, and set up the SDK and CLI by following https://docs.geode-sdk.org/getting-started/ (on Windows that means Visual Studio with the C++ workload, plus CMake).
2. In this folder run:

   ```
   geode build
   ```

3. The build drops `showcase.frame-leniency.geode` into your GD mods folder. Restart GD.

## How it works

| Feature | Hook | What it does |
|---|---|---|
| Hazard grace | `PlayLayer::destroyPlayer` | Counts consecutive frames of deadly contact and only lets the death through once it lasts longer than the grace window. GD's anticheat spike always passes through. |
| Input buffer | `GJBaseGameLayer::handleButton` | A jump release that comes too soon after the press is held back until the minimum hold time has passed. |
| Releasing held taps | `GJBaseGameLayer::processCommands` | Each physics step, sends any held-back releases whose time is up. |
| Cleanup | `PlayLayer::init`, `resetLevel`, `onQuit` | Clears state between attempts. |

Frames here are physics steps (240 per second), counted with `m_gameState.m_currentProgress`.

## Tuning

Settings are in Geode's mod menu: grace window 0–8 frames (default 2), buffer window 0–12 frames (default 2), and a toggle for the on-screen "Leniency" label.
