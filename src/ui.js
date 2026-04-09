/*
 * NoteTwist — Standalone UI
 *
 * Paginated parameter display:
 *   Harmonize / Note / Timing / Delay / Feedback
 *
 * Navigate pages with jog wheel or left/right arrows.
 * Live BPM shown on every page.
 */

import { decodeDelta } from '../../../shared/input_filter.mjs';

const PAGES = [
    {
        title: 'Harmonize',
        params: [
            { key: 'unison',      label: 'Unison' },
            { key: 'octaver',     label: 'Octaver' },
            { key: 'harmonize_1', label: 'Harmony 1' },
            { key: 'harmonize_2', label: 'Harmony 2' }
        ]
    },
    {
        title: 'Note',
        params: [
            { key: 'note_offset',     label: 'Note Ofs' },
            { key: 'gate_time',       label: 'Gate %' },
            { key: 'velocity_offset', label: 'Vel Ofs' }
        ]
    },
    {
        title: 'Timing',
        params: [
            { key: 'octave_shift', label: 'Octave' },
            { key: 'clock_shift',  label: 'Clk Shift' },
            { key: 'bpm',          label: 'BPM' }
        ]
    },
    {
        title: 'Delay',
        params: [
            { key: 'delay_time',   label: 'Dly Time' },
            { key: 'delay_level',  label: 'Dly Level' },
            { key: 'repeat_times', label: 'Repeats' }
        ]
    },
    {
        title: 'Feedback',
        params: [
            { key: 'fb_velocity',    label: 'FB Vel' },
            { key: 'fb_note',        label: 'FB Note' },
            { key: 'fb_note_random', label: 'FB Rand' },
            { key: 'fb_gate_time',   label: 'FB Gate' },
            { key: 'fb_clock',       label: 'FB Clock' }
        ]
    }
];

let page = 0;

function drawUI() {
    clear_screen();

    /* Header */
    print(2, 0, 'NoteTwist', 1);
    const bpm = host_module_get_param('bpm_display') || '120.0';
    const bpmText = bpm + ' BPM';
    const bpmW = text_width(bpmText);
    print(126 - bpmW, 0, bpmText, 1);

    /* Divider */
    fill_rect(0, 10, 128, 1, 1);

    /* Page title */
    const pg = PAGES[page];
    print(2, 13, pg.title, 1);

    /* Divider */
    fill_rect(0, 22, 128, 1, 1);

    /* Parameters */
    const y0 = 25;
    const lineH = 8;
    for (let i = 0; i < pg.params.length; i++) {
        const p = pg.params[i];
        const val = host_module_get_param(p.key) || '—';
        print(2, y0 + i * lineH, p.label, 1);
        const valW = text_width(val);
        print(126 - valW, y0 + i * lineH, val, 1);
    }

    /* Footer */
    fill_rect(0, 56, 128, 1, 1);
    const footer = (page + 1) + '/' + PAGES.length;
    const fw = text_width(footer);
    print(64 - (fw >> 1), 58, footer, 1);
    print(2, 58, '<', 1);
    print(122, 58, '>', 1);
}

globalThis.init = function () {
    page = 0;
    drawUI();
};

globalThis.tick = function () {
    drawUI();
};

globalThis.onMidiMessageInternal = function (data) {
    if (data[0] !== 0xB0 || data[2] === 0) return;
    const cc = data[1];

    if (cc === 14) {
        /* Jog wheel */
        const d = decodeDelta(data[2]);
        if (d) page = (page + d + PAGES.length) % PAGES.length;
    } else if (cc === 63 /* right */) {
        page = (page + 1) % PAGES.length;
    } else if (cc === 62 /* left */) {
        page = (page - 1 + PAGES.length) % PAGES.length;
    }
};

globalThis.onMidiMessageExternal = function () {};
