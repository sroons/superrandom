# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Super Random is an algorithm plugin for the Expert Sleepers disting NT eurorack module. It generates up to 24 channels of triggered random CV with per-channel mode selection (stepped or smooth, bipolar or unipolar). A trigger input fires all channels simultaneously; smooth channels slew toward their target at a configurable rate.

## Build

Requires the `arm-none-eabi` cross-compilation toolchain (ARM Cortex-M7 target).

```
make        # compiles src/*.cpp -> plugins/*.o
make clean  # removes compiled .o files
```

The compiled `.o` file goes in `plugins/` and is copied to the disting NT's SD card at `/programs/plug-ins/`.

## Architecture

Single-file C++11 plugin (`src/superrandom.cpp`) using the distingNT_API (referenced from `../disting_pulsar/distingNT_API/`).

**Core DSP**: In `step()`, trigger detection via rising-edge on the trigger bus generates new random targets for all channels. Stepped modes jump immediately; smooth modes slew toward the target using a one-pole filter with a user-configurable coefficient.

**Parameter layout**: Global params (trigger input, slew rate) plus per-channel params (CV output bus, output mode, random mode enum). Channel count is set via `_NT_specification` at instantiation time. All per-channel pages share a page group so cursor position is preserved when switching between channels.

**Random modes**: Step +/- (bipolar S&H), Smooth +/- (bipolar interpolated), Step 0/+ (unipolar S&H), Smooth 0/+ (unipolar interpolated). Bipolar outputs range +/-5V, unipolar 0-10V.

**PRNG**: Xorshift32 — lightweight, no external dependencies.

## distingNT API Reference

Examples and API headers live at `../disting_pulsar/distingNT_API/`. Key references:
- `include/distingnt/api.h` — full API (v13)
- `examples/gainMultichannel.cpp` — closest pattern to this project (multi-channel with specifications)
- `examples/gain.cpp` — simplest complete algorithm

**Constraints**: C++11 only, no RTTI, no exceptions, `-Os -ffast-math`. `step()` receives audio in 4-frame blocks. Plugin GUIDs must contain at least one capital letter. Bus values are floats where 1.0 = 5V.
