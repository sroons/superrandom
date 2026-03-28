# Super Random

A multi-channel triggered random CV generator algorithm for the [Expert Sleepers disting NT](https://www.expert-sleepers.co.uk/distingNT.html) eurorack module.

## Features

- **Up to 24 CV outputs** (channel count configurable at instantiation)
- **Per-channel random type**: Stepped (sample & hold) or Smooth (slewed interpolation)
- **Per-channel polarity**: Bipolar (+/-) or Unipolar (0/+)
- **Per-channel voltage range**: 0.1V to 10.0V
- **Per-channel looping**: Record 1-64 random steps, then cycle through them (loop buffers persist across preset save/load)
- **Per-channel skip probability**: 0-100% chance of holding the current value on each trigger
- **Global trigger input**: All channels advance on the same trigger
- **Global slew rate**: Controls interpolation speed for smooth channels
- **Real-time OLED display**: Adaptive bar graph with trigger/skip flash, loop position indicators, and mode-differentiated bar styles

## Building

Requires the `arm-none-eabi` cross-compilation toolchain and the [distingNT API](https://github.com/expertsleepersltd/distingNT_API) headers.

```
make        # compiles src/superrandom.cpp -> plugins/superrandom.o
make clean
```

The Makefile expects the API headers at `../disting_pulsar/distingNT_API/`. Override with:

```
make NT_API_PATH=/path/to/distingNT_API
```

## Installation

Copy `plugins/superrandom.o` to the disting NT's SD card at `/programs/plug-ins/`.

## Parameters

### Global

| Parameter | Range | Description |
|-----------|-------|-------------|
| Trigger | CV input bus | Rising edge (>1V) advances all channels |
| Slew | 1-1000% | Interpolation speed for smooth channels |

### Per-Channel

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| CV output | Bus 1-64 | Bus 13+ | Output bus assignment |
| Mode | Add/Replace | Replace | Output bus mode |
| Type | Stepped/Smooth | Stepped | S&H jump or slewed interpolation |
| Polarity | Bipolar/Unipolar | Bipolar | +/- range or 0/+ range |
| Range | 0.1V - 10.0V | 5.0V | Maximum output voltage |
| Loop | 0-64 | 0 (off) | Number of steps to record and loop |
| Skip % | 0-100% | 0% | Probability of skipping a trigger |

## Display

The OLED shows a bar graph of all channel outputs with adaptive layout:

- **Filled bars** = stepped channels, **hollow bars** = smooth channels
- **Bright flash** on trigger, **dim flash + X marker** on skip
- **Loop position**: dot array (wide layout) or progress bar (narrow layout) below each looping channel
- **Channel numbers** shown when space permits

## License

MIT
