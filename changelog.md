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
