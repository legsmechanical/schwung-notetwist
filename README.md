# NoteTwist

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

NoteTwist is a chainable MIDI FX module for [Schwung](https://github.com/charlesvestal/schwung) on Ableton Move. It combines an octave transposer, harmonizer, note processor, clock-synced note shifter, and MIDI delay with per-repeat feedback into a single effect chain. The design is inspired by the Play Effects and MIDI Delay sections of the Yamaha RS7000 hardware sequencer.

## Signal Chain

Notes pass through five processing stages in order:

```
Input
  |
  v
[1. Octave Transposer]  Shift by octaves
  |
  v
[2. Harmonize]           Add unison, octave, and harmony notes
  |
  v
[3. Note Page]           Offset pitch, scale gate time, adjust velocity
  |
  v
[4. Clock Shift]         Delay notes by a musical time value
  |
  v
[5. MIDI Delay]          Echoing repeats with pitch/velocity/timing feedback
  |
  v
Output
```

## Parameters

### Stage 1 — Octave Transposer

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `octave_shift` | -4 to +4 | 0 | Transposes all notes by this many octaves (multiplied by 12 semitones) before any other processing. |

### Stage 2 — Harmonize

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `unison` | OFF, x2, x3 | OFF | Adds 1 or 2 copies of each note with ~5ms stagger for a thickening effect. |
| `octaver` | -4 to +4 | 0 | Adds one note this many octaves above or below the original. 0 = off. |
| `harmonize_1` | -24 to +24 | 0 | Adds a harmony note at this semitone interval. 0 = off. |
| `harmonize_2` | -24 to +24 | 0 | Adds a second harmony note at this semitone interval. 0 = off. |

All added notes mirror the note-off of their corresponding note-on.

### Stage 3 — Note Page

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `note_offset` | -24 to +24 | 0 | Shifts all notes by this many semitones (applied after octave transposer). |
| `gate_time` | 0 to 200 | 100 | Scales note duration as a percentage. 100 = no change, 50 = half length, 200 = double length. |
| `velocity_offset` | -127 to +127 | 0 | Added to incoming note velocity. Result is clamped to 1-127. |

### Stage 4 — Clock Shift

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `clock_shift` | 0, 1/64, 1/32, 1/16T, 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2, 1/1 | 0 | Delays all notes by a musical interval. Both note-on and note-off are shifted equally to preserve gate time. |

### Stage 5 — MIDI Delay

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `delay_time` | 0, 1/64, 1/32, 1/16T, 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2, 1/1 | 1/4 | Time between delay repeats. |
| `delay_level` | 0 to 127 | 80 | Velocity of the first repeat as a fraction of the original: `repeat_vel = original_vel * delay_level / 127`. |
| `repeat_times` | 0 to 64 | 3 | Number of echo repeats. 0 = delay off. |

#### Delay Feedback

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `fb_velocity` | -127 to +127 | -10 | Added to each successive repeat's velocity. Negative values create a natural fade-out. |
| `fb_note` | -24 to +24 | 0 | Semitones added cumulatively to each repeat's pitch. 0 = no pitch shift. |
| `fb_note_random` | off, on | off | When on, overrides `fb_note` with a random shift between -12 and +12 semitones per repeat. |
| `fb_gate_time` | -100 to +100 | 0 | Gate time multiplier per repeat: `repeat_gate = prev_gate * (1 + fb_gate_time/100)`. |
| `fb_clock` | -100 to +100 | 0 | Delay time multiplier per repeat: `repeat_delay = prev_delay * (1 + fb_clock/100)`. Positive values create accelerating echoes. |

#### Other

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `bpm` | 20 to 300 | 120 | Fallback BPM used when no MIDI clock is available. |

## Tempo Sync

NoteTwist automatically locks to the Move's internal MIDI clock when the sequencer is running. It measures the interval between incoming 0xF8 clock messages over a rolling 24-tick window to derive live BPM. All timing calculations (clock shift, delay time) use this live tempo.

When the sequencer is stopped or no clock messages have arrived in the last 2 seconds, the module falls back to the host's tempo query, then to the user-settable `bpm` parameter.

## Navigation

NoteTwist's Shadow UI has three menu levels:

### Basic (Root)

The main screen with the most-used parameters on knobs 1-8:

1. Octave, 2. Note Offset, 3. Velocity Offset, 4. Gate Time, 5. Delay Time, 6. Repeats, 7. Clock Shift, 8. Delay Level

Plus navigation links to the two sub-pages.

### Harmonize

Accessed from "Harmonize" in the root menu. Contains unison, octaver, and the two harmony interval controls on knobs 1-4.

### Delay Detail

Accessed from "Delay Detail" in the root menu. Contains all delay and feedback parameters on knobs 1-8: delay time, delay level, repeats, and the five feedback controls.

## Tips

- **Rising pitch echoes**: Set `fb_note` to +3 or +5 with 4-6 repeats for arpeggiated delay tails that climb through intervals.

- **Instant triads**: Set `harmonize_1` to +4 (major third) and `harmonize_2` to +7 (fifth) to turn every note into a major triad. Use +3 and +7 for minor.

- **Rhythmic stutter**: Use `clock_shift` at 1/16 or 1/16T with `gate_time` at 30-50% for tight rhythmic patterns that play behind the beat.

- **Dub delay**: Set `delay_time` to 1/4, `repeat_times` to 8-12, `fb_velocity` to -5, and `fb_clock` to +10 for gradually stretching echo tails.

- **Random scatter**: Enable `fb_note_random` with 6+ repeats and `fb_velocity` at -15 for delay tails that spray notes unpredictably across the keyboard.

## Credits

NoteTwist was built on the [Schwung](https://github.com/charlesvestal/schwung) framework by [@charlesvestal](https://github.com/charlesvestal), inspired by the Play Effects and MIDI Delay of the Yamaha RS7000 hardware sequencer.

## License

MIT License. See [LICENSE](LICENSE) for details.
