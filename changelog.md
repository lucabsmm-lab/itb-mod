# v1.0.9
- The mod's look-ahead guards now run before every other mod's hooks (e.g. Mega Hack), so other mods never see the hidden look-ahead players or their deaths. Fixes look-ahead deaths going unnoticed (a deadly jump shown as safe) and other mods' hitbox / death features firing while you are alive.
- The look-ahead now goes through gamemode, teleport, gravity, speed and size portals instead of stopping at them. Level-wide changes (camera, portal bounds) are saved before and restored after every look-ahead; ground / dual animations, mirror effects, camera shake and particle triggers are blocked during it.
- Dual mode: gamemode portals still end the look-ahead (GD switches both players there).
- Safety net: if a real player's gamemode, gravity, size or speed changes during a look-ahead, it is put back.

# v1.0.8
- Orbs: the look-ahead now really plans clicking the next orb (it was skipping every orb it touched, so orb chains never got planned).
- Your press is treated as a hold, like in the real game: holding into an orb fires it, so clicking early into the wrong orb (e.g. the green one before the blue one) is caught and held back.
- Only orbs you are touching on the current tick can be clicked in the look-ahead (matches GD).
- Show hitboxes on death: GD's hitbox drawing is blocked during a look-ahead, its level flags are restored after every look-ahead, and a failsafe turns hitboxes back off if GD thinks a player died when no real player did.
- Holding into a landing: the touchdown is detected a few ticks earlier, and letting go also cancels GD's buffered jump.

# v1.0.7
- Holding through a landing: GD's automatic jump on touchdown is now buffered too. If it would doom you, the mod lets go for you and presses again on the first safe tick (you keep holding the real button).

# v1.0.6
- Fixed "show hitboxes on death" turning on while you were still alive (a hidden look-ahead cube dying set GD's death flag).
- Every timing option is now simulated from your real cube instead of a saved copy, and copies keep their collision memory. Fixes held-back jumps right at a landing coming out 1 tick early.
- Uses far fewer hidden players.

# v1.0.5
- Held-back inputs are now handed to the game at the same point GD processes real clicks (they could land 1 tick off, which is exactly the doomed timing).
- Cube / ball / spider: your click is treated as a tap, so the next orb can be planned right away (dense orb chains no longer look like dead ends). Robot keeps the assumed hold.
- Plans up to 3 follow-up clicks deep, and tries 3 click timings per orb.
- Debug: drift check now says whether a held-back input landed on time, 1 tick early or 1 tick late, and logs where it was released.

# v1.0.4
- Cube / ball / robot / spider: the look-ahead now plans your next clicks like a player would. At every orb you pass and every landing it tries clicking there, up to 2 clicks deep; reaching your next chance to click alive counts as safe. Orb chains no longer look like dead ends.
- A one-tick surface touch (e.g. during an orb flip) no longer counts as a landing.
- Debug: the green line now includes the planned follow-up clicks.

# v1.0.3
- Fixed landings not being detected in the look-ahead (it thought every jump never landed, so almost every input looked doomed and nothing got buffered).
- Debug: a doomed input that nothing can save now shows only the red line.

# v1.0.2
- Held-back inputs now land on the exact tick the look-ahead chose (they were firing 1 tick early).
- Look-ahead saves the "do nothing" path once and branches from it, so it searches far more timings in the same time.
- Landing no longer counts as safe by itself: it must still be possible to stand still or jump out from there.
- Cube / ball / spider only test the continuations that matter for them (faster).

# v1.0.1
- Look-ahead has a hard time limit (no more freezes when an input is buffered).
- Hidden player keeps its own object ID; real players are checked and restored after every look-ahead.
- Debug Mode: draws predicted paths and logs every decision plus prediction drift.

# v1.0.0
- First release.
