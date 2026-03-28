# Code Review — 2026-03-28 12:53:37
## Target: `src/`

## Issues

### [MAJOR] PRNG seeded with fixed constant — all instances produce identical sequences
- **Location**: src/superrandom.cpp:222
- **Issue**: `rng.seed(42)` means every instance of the algorithm generates the exact same "random" sequence. Multiple instances running in parallel will output identical CV. Even a single instance will repeat the same sequence every time the module powers on (unless a preset is loaded).
- **Suggestion**: Seed from a varying source — e.g., use `NT_getSystemTicks()` or similar API call, or combine the instance pointer address with a counter, to ensure each instantiation gets a unique sequence.

### [MAJOR] Serialising PRNG state as signed int truncates/mangles high bit
- **Location**: src/superrandom.cpp:692
- **Issue**: `stream.addNumber((int)pThis->rng.state)` casts `uint32_t` to `int`. When bit 31 is set, this produces a negative number. On deserialise (line 738), the negative int is cast back to `uint32_t`, which happens to round-trip correctly via two's complement on most platforms, but this is implementation-defined behavior in C++11 for signed-to-unsigned conversion of negative values read from JSON. If the JSON parser interprets the negative number differently, state corruption occurs.
- **Suggestion**: Serialise as two 16-bit halves, or as an unsigned value if the JSON API supports it, to avoid relying on implementation-defined signed overflow behavior.

### [MAJOR] Loop position not reset when loop finishes filling
- **Location**: src/superrandom.cpp:410-425
- **Issue**: When filling the loop buffer, `loopPos` is never set. It remains at whatever value it had (0 from init, or stale from a previous loop length). Once `loopFilled == loopLen`, the loop playback starts reading from the stale `loopPos` rather than from index 0. If a user changes loop length after some triggers, `loopPos` could be out of range for the new `loopLen`, and the modulo on line 430 would wrap it, but the playback order would not start from the beginning of the recorded buffer.
- **Suggestion**: Set `cs.loopPos = 0` when `loopFilled` reaches `loopLen` (i.e., on the transition from recording to looping), or at least when `loopFilled` is incremented to equal `loopLen`.

### [MINOR] `goto` across loop iteration used for skip logic
- **Location**: src/superrandom.cpp:387-437
- **Issue**: The `goto write_output` label jumps to the bottom of a `for` loop body. While technically valid C++, this is unusual, harder to follow, and could mask bugs if the loop body is modified later. The label `write_output` doesn't actually write output — it's just the end of the channel loop body.
- **Suggestion**: Replace with `continue` to skip to the next channel iteration, which is semantically clearer and achieves the same result.

### [MINOR] Slew rate is sample-rate dependent
- **Location**: src/superrandom.cpp:309-311, 448-449
- **Issue**: The slew coefficient is a fixed value applied per sample (`currentValue += diff * slewCoeff`). If the disting NT ever changes sample rate, or if the API runs `step()` at different rates, the slew behavior changes. The "Slew" parameter percentage would mean different things at different sample rates.
- **Suggestion**: Consider deriving the coefficient from the sample rate (available via the API) so slew behavior is consistent. This may be acceptable if the sample rate is guaranteed fixed on this platform.

### [MINOR] Trigger threshold assumes 1V but bus scale is 1.0 = 5V
- **Location**: src/superrandom.cpp:365
- **Issue**: The trigger threshold is `trigIn[i] > 1.0f`. Given bus values where 1.0 = 5V, this means the trigger fires at 5V. This is a valid eurorack trigger level, but the comment on line 362 says "rising edge above 1V" which is misleading — it's 1.0 in bus units, which is 5V.
- **Suggestion**: Update the comment to say "rising edge above 5V (1.0 bus units)" or define a named constant like `kTriggerThreshold` with a comment explaining the voltage. Alternatively, if you intended a 1V threshold, use `0.2f`.

### [MINOR] No bounds check on `loopPos` after deserialisation
- **Location**: src/superrandom.cpp:779-781
- **Issue**: `loopFilled` is clamped to `kMaxLoopSteps` after deserialisation (line 790-791), but `loopPos` is not validated. A corrupted or hand-edited preset could set `loopPos` to a value >= `kMaxLoopSteps`, causing an out-of-bounds read on `loopBuffer` at line 429.
- **Suggestion**: Clamp `loopPos` similarly: `if (cs.loopPos >= kMaxLoopSteps) cs.loopPos = 0;`

### [NIT] `kMaxBusses` typo
- **Location**: src/superrandom.cpp:21
- **Issue**: "Busses" is a less common spelling; "Buses" is standard in both American English and technical usage.
- **Suggestion**: Rename to `kMaxBuses` for consistency, or leave as-is if matching the disting NT API naming convention.

### [NIT] `write_output` label is misleadingly named
- **Location**: src/superrandom.cpp:437
- **Issue**: The label `write_output` doesn't write any output — it's the bottom of the trigger-handling block. The `(void)0;` is a no-op to satisfy the requirement for a statement after a label.
- **Suggestion**: If keeping the goto pattern, rename to `skip_channel` or `next_channel` to reflect intent.

## Strengths

- Clean, well-organized single-file structure with logical sections and clear commenting.
- Good use of the disting NT API patterns — parameter pages, page groups, specifications, and serialisation are all handled correctly.
- The adaptive draw() function handles varying channel counts gracefully with both wide and narrow layouts.
- Efficient DSP: per-channel parameters are cached outside the sample loop, and the inner loop is tight.
- Serialisation/deserialisation is robust — handles mismatched channel counts and unknown fields gracefully with skip logic.
- The xorshift32 PRNG is a good choice for this use case: lightweight, fast, and sufficient randomness for CV generation.

## Summary

This is a well-structured and readable disting NT plugin that demonstrates solid understanding of the platform API. The most significant issues are the fixed PRNG seed (which defeats the purpose of a "random" module when multiple instances are used or across power cycles) and the unclamped `loopPos` on deserialisation (which could cause an out-of-bounds memory read from a malformed preset). The loop position tracking during the fill-to-playback transition also needs attention. With these fixes applied, the code is in good shape. **Request changes.**

---
## Author Response — 2026-03-28 12:53:37

### [MAJOR] PRNG seeded with fixed constant — all instances produce identical sequences
- **Response**: Agree — this was placeholder code that should have been replaced before shipping. Multiple instances would indeed produce identical output, and sequences repeat across power cycles.
- **Action**: Changed: seed is now `NT_getCpuCycleCount() ^ (uint32_t)(uintptr_t)self`, combining a hardware cycle counter (varies each instantiation) with the instance address (unique per instance). This ensures both cross-instance and cross-boot uniqueness.

### [MAJOR] Serialising PRNG state as signed int truncates/mangles high bit
- **Response**: Agree — the `addNumber` API only offers `int` and `float` overloads, so casting `uint32_t` to `int` with bit 31 set is indeed implementation-defined in C++11. While two's complement round-trips on ARM, relying on this is fragile.
- **Action**: Changed: PRNG state is now serialised as two 16-bit halves (`rngHi` and `rngLo`), both guaranteed to fit in a positive `int`. Deserialisation handles both the new format and the legacy single `rng` field for backward compatibility with existing presets.

### [MAJOR] Loop position not reset when loop finishes filling
- **Response**: Agree — `loopPos` was initialised to 0 in `construct()` and reset when the loop-steps parameter changes, so in practice it would almost always be 0 at the fill-to-playback transition. However, that's coincidental rather than guaranteed, and an explicit reset is the correct approach.
- **Action**: Changed: `cs.loopPos = 0` is now set when `loopFilled` reaches `loopLen`.

### [MINOR] `goto` across loop iteration used for skip logic
- **Response**: Agree — `continue` is the idiomatic and clearer choice here. The `goto` was a leftover from an earlier version where there was post-loop-body code that needed to run for skipped channels, but that code was removed and the `goto` should have been converted to `continue` at that point.
- **Action**: Changed: replaced `goto write_output` with `continue` and removed the `write_output` label and `(void)0` no-op.

### [MINOR] Slew rate is sample-rate dependent
- **Response**: Disagree — the disting NT runs at a fixed 48kHz sample rate (per the platform specification), and the API does not support variable sample rates for algorithm plugins. The `sampleRate` field in `NT_globals` is available but is always 48000. Deriving the coefficient from the sample rate would add complexity for a scenario that cannot occur on this platform.
- **Action**: No change: the platform guarantees a fixed 48kHz sample rate.

### [MINOR] Trigger threshold assumes 1V but bus scale is 1.0 = 5V
- **Response**: Agree on the misleading comment. The threshold of 1.0 bus units (= 5V) is intentional — this is the standard eurorack trigger/gate level. But the comment was wrong.
- **Action**: Changed: updated comment to "rising edge above 5V / 1.0 bus units".

### [MINOR] No bounds check on `loopPos` after deserialisation
- **Response**: Agree — a corrupted preset could cause an out-of-bounds read. Good catch.
- **Action**: Changed: added `if ( cs.loopPos >= kMaxLoopSteps ) cs.loopPos = 0;` immediately after deserialising `loopPos`.

### [NIT] `kMaxBusses` typo
- **Response**: Disagree — "busses" is an accepted variant spelling and matches the naming convention used elsewhere in disting NT plugin code. Renaming would introduce inconsistency with the broader ecosystem.
- **Action**: No change: keeping `kMaxBusses` for ecosystem consistency.

### [NIT] `write_output` label is misleadingly named
- **Response**: Agree — but moot since the label has been removed entirely as part of the `goto` -> `continue` fix.
- **Action**: Changed: label removed (see `goto` fix above).

## Overall Notes
All three MAJOR issues and most MINOR issues have been addressed. The build compiles cleanly with no warnings. The serialisation change is backward-compatible with existing presets (the legacy `"rng"` field is still handled on deserialise).
