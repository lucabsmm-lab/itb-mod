# Input Buffer (Geode mod for GD 2.2081)

Holds back early clicks **and** releases by up to 2-16 ticks (1 tick = 1/240 s) so they land on a tick you survive. Inputs that are already fine are never touched.

## How it decides

When you press or release, the mod copies your player into a hidden "ghost" and runs the game's own physics and collisions forward:

1. **On-time check:** assume you keep the new button state for *Assumed Hold* ticks (default 12), then try several ways of continuing (letting go right away, at +6, +12, +24, +48 ticks, or never). If any of them survives until you are back in control, the input goes through untouched.
   - Cube / ball / robot / spider: "back in control" = touching ground again.
   - Ship / UFO / wave / swing: survive *Fly Survive Window* ticks (default 30) after your next input.
2. **Doomed:** try the same input 1, 2, 3 ... ticks later (up to your setting).
   - **Earliest**: use the first tick that survives.
   - **Safest** (default, "path of least resistance"): among the first few safe ticks, pick the one that leaves the most ways to keep going.
3. If nothing in range saves you, the input happens normally.

Dual with one key: a timing counts only if **both** players survive. 2-player mode: each player is buffered separately. Platformer levels are ignored.

The look-ahead stops (and counts as safe) at gamemode, dual, mirror and teleport portals, so it won't save inputs whose death only happens after one of those.

## Controls

| Action | Where |
| --- | --- |
| Settings popup | **IB** button in the pause menu, the online level page, and the My Levels level page |
| On / Off | `F1` / `F2` |
| 2 / 4 / 6 / 8 ticks | `0+2` / `0+4` / `0+6` / `0+8` (press together) |
| 10 / 12 / 14 / 16 ticks | `1+0` / `1+2` / `1+4` / `1+6` |

**Safe Mode** (on by default): any attempt where the buffer was on is treated like a start-pos test run, so no progress, completions, stars, orbs or leaderboard scores are saved.

## Building

Push this repo to GitHub. The **Build Geode Mod** action in `.github/workflows/build.yml` builds it. Download the **Build Output** artifact from the finished run, unzip it, and put the `.geode` file in your `Geometry Dash/geode/mods` folder.
