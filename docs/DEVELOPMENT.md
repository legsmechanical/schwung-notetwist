# NoteTwist Development Guide

## Project Structure

```
src/modules/midi_fx/notetwist/
  module.json          # Module metadata, ui_hierarchy, capabilities
  ui.js                # Standalone UI (paginated parameter display)
  help.json            # On-device help content
  dsp/
    notetwist.c        # All DSP logic (single-file C implementation)
  docs/
    PARAMETERS.md      # Complete parameter reference
    DEVELOPMENT.md     # This file
```

### Key Files

**`dsp/notetwist.c`** — The entire DSP implementation in ~500 lines of C. Contains:
- Instance struct (`notetwist_t`) with all parameters, event queue, and note tracking
- `rs_process()` — Handles incoming MIDI, applies all processing stages, schedules delayed events
- `rs_tick()` — Called every audio block (~2.9ms), fires scheduled events from the queue
- `rs_set()` / `rs_get()` — Parameter get/set including JSON state serialization
- Event queue (sorted array) and note tracking (128-entry table)

**`module.json`** — Defines the module identity, `chain_params` (parameter metadata for the Shadow UI parameter editor), and `ui_hierarchy` (menu structure with knob assignments).

**`ui.js`** — A simple standalone display with 5 pages (Harmonize / Note / Timing / Delay / Feedback). Primarily used for standalone loading; the Signal Chain Shadow UI renders parameters from `ui_hierarchy` and `chain_params`.

## How the Event Queue Works

All time-delayed MIDI output goes through a sorted event queue:

```c
typedef struct {
    uint64_t fire_at;   // Sample count when this event should fire
    uint8_t  msg[3];    // MIDI message bytes
    uint8_t  len;       // Message length
} sched_event_t;
```

- **Capacity**: 2048 events (`MAX_EVENTS`). Events beyond this are silently dropped.
- **Insertion**: Binary search for sorted position, `memmove` to make room. O(n) worst case, bounded by array size.
- **Firing**: In `rs_tick()`, events are popped from the front while `fire_at <= sample_counter`, up to `max_out` (typically 16) per tick. Unfired events remain in the queue for the next tick.
- **Ordering**: Events with the same `fire_at` time fire in insertion order.

Events are created by:
- **Clock shift**: Note-on/off delayed by clock_shift amount
- **Unison stagger**: Copies delayed by ~220 samples per copy
- **Gate time**: Note-offs rescheduled when gate_time != 100
- **MIDI delay**: All repeat note-ons scheduled at note-on time; repeat note-offs scheduled at note-off time

### Note Tracking

An `active_note_t` array (128 entries, indexed by original MIDI note number) tracks:
- Which generated notes (transposed + harmonies) were sent for each input note
- The note-on timestamp and velocity
- Delay repeat parameters snapped at note-on time (so note-off processing uses consistent values even if parameters change mid-note)
- Per-repeat pitch offsets, velocities, and gate factors

This ensures note-offs always match their corresponding note-ons, even through transposition, harmonization, and delay feedback.

## How MIDI Clock Sync Works

The module counts incoming 0xF8 system real-time messages in `rs_process()`:

1. Each 0xF8 timestamp is stored in a rolling window of 24 entries (one quarter note at 24 PPQN).
2. BPM is computed from the average interval between the oldest and newest timestamps in the window.
3. If no clock tick arrives within 2 seconds (88200 samples), the live BPM is considered stale.
4. Fallback chain: live clock BPM -> host API `get_bpm()` -> user `bpm` parameter.

Clock messages (0xF8, 0xFA, 0xFB, 0xFC) are consumed and never passed through to the output.

## Building Locally

Docker is required for cross-compilation to ARM64 (Move's architecture).

```bash
# From the schwung repo root:
./scripts/build.sh
```

This compiles `notetwist.c` to `build/modules/midi_fx/notetwist/dsp.so` along with all other modules. The build script uses `needs_rebuild()` for incremental compilation.

To build only after changing notetwist files, the full build script still runs but skips unchanged modules.

## Deploying to Move

```bash
# Full deploy (recommended):
./scripts/install.sh local --skip-modules --skip-confirmation

# This deploys the host binary, all built-in modules (including notetwist),
# shared files, and restarts the Move service.
```

The `--skip-modules` flag skips downloading external modules from the Module Store. The `--skip-confirmation` flag skips the interactive confirmation prompt.

**Important**: Never scp individual files to the device. The install script handles setuid permissions, symlinks, feature configuration, and service restart.

## Reading the Debug Log

```bash
# Enable logging (creates a flag file):
ssh ableton@move.local 'touch /data/UserData/schwung/debug_log_on'

# Tail the log:
ssh ableton@move.local 'tail -f /data/UserData/schwung/debug.log'

# Disable logging:
ssh ableton@move.local 'rm -f /data/UserData/schwung/debug_log_on'
```

Look for:
- Module load/unload messages from the chain host
- Parameter set/get calls
- Any segfaults or crashes (the log captures stderr)

In JavaScript, use `console.log()` which auto-routes to the debug log. In C, use `g_host->log()` if the host pointer is available.

## Known Limitations

- **No sub-block timing**: `process_midi()` does not receive a sample-accurate timestamp. Events scheduled at the current `sample_counter` fire on the next `tick()` call, adding up to 128 samples (~2.9ms) of latency.

- **Event queue overflow**: With extreme settings (64 repeats, full harmonization, unison x3), the 2048-event queue can fill up. Events that don't fit are silently dropped. In practice this only occurs with deliberately extreme parameter combinations.

- **Unison on same pitch**: MIDI has no concept of polyphonic voices on the same note number within a channel. Unison copies at the same pitch rely on the ~5ms stagger for their thickening effect; the note-off turns off all copies at once.

- **Parameter changes mid-note**: Delay repeat parameters are snapped at note-on time and used for the corresponding note-offs. Changing parameters while notes are held affects new notes but not in-flight delay repeats.

- **No negative clock shift**: The clock_shift parameter only delays notes; it cannot advance them before they arrive.

- **Integer division for BPM**: The rolling clock window averages over integer sample counts, which can introduce slight tempo jitter at very high BPMs (>250).

## Contributing

1. Fork the [schwung](https://github.com/charlesvestal/schwung) repository
2. Create a feature branch: `git checkout -b notetwist/my-feature`
3. Make your changes in `src/modules/midi_fx/notetwist/`
4. Build and test on hardware: `./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation`
5. Enable debug logging and verify no runtime errors
6. Open a pull request against `main`

Keep changes focused. If modifying the DSP, test with:
- Simple note passthrough (all defaults)
- Harmonization (verify note-offs match)
- Delay with feedback (verify repeats fade correctly)
- Clock sync (play with sequencer running, then stop it)
