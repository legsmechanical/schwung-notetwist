# NoteTwist Parameter Reference

Complete reference for all NoteTwist parameters, organized by signal chain stage.

---

## Stage 1 — Octave Transposer

### octave_shift

- **Display Name**: Octave
- **Key**: `octave_shift`
- **Type**: int
- **Range**: -4 to +4
- **Default**: 0
- **Step**: 1

Transposes all incoming notes by the specified number of octaves (value * 12 semitones). Applied before all other processing stages. A value of 0 passes notes through unchanged.

**Edge cases**: If the transposed note falls outside MIDI range 0-127, it is clamped to the nearest valid value. A note at C1 (36) with octave_shift -4 would clamp to 0 rather than going negative.

---

## Stage 2 — Harmonize

### unison

- **Display Name**: Unison
- **Key**: `unison`
- **Type**: enum
- **Options**: OFF, x2, x3
- **Default**: OFF

Sends additional copies of each note with a small staggered delay (~5ms per copy) for a thickening/chorus-like effect.

- **OFF**: No additional notes.
- **x2**: One extra copy, staggered by ~220 samples (~5ms).
- **x3**: Two extra copies, staggered by ~220 and ~440 samples (~5ms and ~10ms).

All unison copies use the same pitch and velocity as the original. Note-offs are timed to cover the latest copy, extending the note duration by the total stagger amount.

### octaver

- **Display Name**: Octaver
- **Key**: `octaver`
- **Type**: int
- **Range**: -4 to +4
- **Default**: 0
- **Step**: 1

Adds a single note at the specified number of octaves above (positive) or below (negative) the transposed note.

- **0**: Off, no additional note.
- **Positive values**: Octave(s) up.
- **Negative values**: Octave(s) down.

The added note is dropped silently if it falls outside MIDI range 0-127.

### harmonize_1

- **Display Name**: Harmony 1
- **Key**: `harmonize_1`
- **Type**: int
- **Range**: -24 to +24
- **Default**: 0
- **Step**: 1
- **Unit**: semitones

Adds one harmony note at the specified semitone interval relative to the transposed note.

- **0**: Off, no harmony note.
- **+7**: A perfect fifth above.
- **-12**: One octave below (equivalent to octaver -1, but can be combined).

The added note is dropped silently if it falls outside MIDI range 0-127.

### harmonize_2

- **Display Name**: Harmony 2
- **Key**: `harmonize_2`
- **Type**: int
- **Range**: -24 to +24
- **Default**: 0
- **Step**: 1
- **Unit**: semitones

Adds a second harmony note, independent of harmonize_1. Same behavior and edge cases as harmonize_1.

**Combining harmonize_1 and harmonize_2**: Using both creates three-note chords from single notes (original + two harmonies). For example, harmonize_1=+4, harmonize_2=+7 produces a major triad.

---

## Stage 3 — Note Page

### note_offset

- **Display Name**: Note Ofs
- **Key**: `note_offset`
- **Type**: int
- **Range**: -24 to +24
- **Default**: 0
- **Step**: 1
- **Unit**: semitones

Shifts all notes (including harmonies) by this many semitones. Applied after the octave transposer, so the total transposition is `octave_shift * 12 + note_offset`. Useful for fine-tuning pitch in semitone increments where octave_shift provides coarse control.

Results are clamped to 0-127.

### gate_time

- **Display Name**: Gate %
- **Key**: `gate_time`
- **Type**: int
- **Range**: 0 to 200
- **Default**: 100
- **Step**: 1
- **Unit**: percent

Scales the duration of each note. The original duration is measured from note-on to note-off, then multiplied by `gate_time / 100`.

- **100**: No change to note duration.
- **50**: Notes are half their original length (staccato).
- **200**: Notes are doubled in length (legato/overlap).
- **0**: Notes are effectively zero-length (note-off fires immediately).

When the scaled note-off time is in the past (gate_time < 100 for very short original notes), the note-off fires immediately.

### velocity_offset

- **Display Name**: Vel Ofs
- **Key**: `velocity_offset`
- **Type**: int
- **Range**: -127 to +127
- **Default**: 0
- **Step**: 1

Added to the incoming note velocity before any further processing. The result is clamped to 1-127 (never 0, which would be interpreted as note-off in MIDI).

- **Positive values**: Boost velocity (louder/brighter).
- **Negative values**: Reduce velocity (softer/darker).
- **Edge case**: An incoming velocity of 10 with velocity_offset -20 clamps to 1, not 0.

---

## Stage 4 — Clock Shift

### clock_shift

- **Display Name**: Clk Shift
- **Key**: `clock_shift`
- **Type**: enum
- **Options**: 0, 1/64, 1/32, 1/16T, 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2, 1/1
- **Default**: 0 (off)

Delays all notes by a musical time interval. Both note-on and note-off are delayed by the same amount, preserving the original gate time.

Internal clock values (at 480 PPQN): 0=off, 1/64=30, 1/32=60, 1/16T=80, 1/16=120, 1/8T=160, 1/8=240, 1/4T=320, 1/4=480, 1/2=960, 1/1=1920 clocks.

The actual delay in samples depends on the current BPM: `delay_samples = clocks * sample_rate * 60 / (bpm * 480)`.

---

## Stage 5 — MIDI Delay

### delay_time

- **Display Name**: Dly Time
- **Key**: `delay_time`
- **Type**: enum
- **Options**: 0, 1/64, 1/32, 1/16T, 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2, 1/1
- **Default**: 1/4 (index 8)

The time interval between delay repeats, using the same musical time values and clock conversion as clock_shift. Set to 0 to disable the delay regardless of other delay settings.

### delay_level

- **Display Name**: Dly Level
- **Key**: `delay_level`
- **Type**: int
- **Range**: 0 to 127
- **Default**: 80
- **Step**: 1

Controls the velocity of the first repeat as a proportion of the original note's velocity: `first_repeat_vel = original_vel * delay_level / 127`.

- **127**: First repeat at full velocity.
- **80** (default): First repeat at ~63% of original velocity.
- **0**: Delay is effectively silent (no repeats generated).

### repeat_times

- **Display Name**: Repeats
- **Key**: `repeat_times`
- **Type**: int
- **Range**: 0 to 64
- **Default**: 3
- **Step**: 1

Number of echo repeats. Set to 0 to disable the delay entirely.

Repeats are capped internally at 64. Cumulative delay is also capped at 30 seconds to prevent runaway scheduling with extreme feedback settings.

### fb_velocity

- **Display Name**: FB Vel
- **Key**: `fb_velocity`
- **Type**: int
- **Range**: -127 to +127
- **Default**: -10
- **Step**: 1

Added to each successive repeat's velocity (after the first).

- **Negative values** (typical): Each repeat gets quieter, creating a natural fade-out.
- **Positive values**: Each repeat gets louder (use with caution).
- **0**: All repeats at the same velocity (set by delay_level).

Velocity is clamped to 1-127 at each step. Once a repeat hits 1, subsequent repeats stay at 1.

### fb_note

- **Display Name**: FB Note
- **Key**: `fb_note`
- **Type**: int
- **Range**: -24 to +24
- **Default**: 0
- **Step**: 1
- **Unit**: semitones

Pitch shift applied cumulatively to each repeat. The first repeat shifts by `fb_note`, the second by `2 * fb_note`, etc.

- **0**: No pitch change between repeats.
- **+12**: Each repeat is one octave higher.
- **+7**: Each repeat rises by a perfect fifth.

Note numbers are clamped to 0-127. When `fb_note_random` is on, this parameter is ignored.

### fb_note_random

- **Display Name**: FB Rand
- **Key**: `fb_note_random`
- **Type**: enum
- **Options**: off, on
- **Default**: off

When enabled, overrides `fb_note` with a random semitone shift between -12 and +12 for each repeat independently. The random values are generated per-repeat and stored so that note-offs match their corresponding note-ons.

**Interaction with fb_note**: When fb_note_random is on, the fb_note parameter is completely ignored. The cumulative pitch offset is the sum of all random values up to that repeat.

### fb_gate_time

- **Display Name**: FB Gate
- **Key**: `fb_gate_time`
- **Type**: int
- **Range**: -100 to +100
- **Default**: 0
- **Step**: 1
- **Unit**: percent

Gate time multiplier applied cumulatively per repeat: `repeat_gate = prev_gate * (1 + fb_gate_time / 100)`.

- **0**: All repeats have the same gate as the original (after gate_time scaling).
- **-50**: Each repeat's gate is half the previous one (rapid shortening).
- **+50**: Each repeat's gate is 1.5x the previous one (lengthening tails).

### fb_clock

- **Display Name**: FB Clock
- **Key**: `fb_clock`
- **Type**: int
- **Range**: -100 to +100
- **Default**: 0
- **Step**: 1
- **Unit**: percent

Delay interval multiplier applied per repeat: `repeat_delay = prev_delay * (1 + fb_clock / 100)`.

- **0**: Evenly spaced repeats.
- **+50**: Each gap is 1.5x the previous (decelerating echoes).
- **-30**: Each gap is 0.7x the previous (accelerating echoes).

The delay interval is clamped to a minimum of 1 sample to prevent division-by-zero or negative delays. Cumulative delay is capped at 30 seconds.

---

## General

### bpm

- **Display Name**: BPM
- **Key**: `bpm`
- **Type**: int
- **Range**: 20 to 300
- **Default**: 120
- **Step**: 1

Fallback tempo used when no MIDI clock is available. The priority order for BPM is:

1. Live BPM derived from incoming 0xF8 clock messages (if received within the last 2 seconds)
2. Host BPM (queried from the Schwung host API)
3. This parameter
