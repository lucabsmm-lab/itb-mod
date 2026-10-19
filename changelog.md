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
