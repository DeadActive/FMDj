// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE (from Hortator's simulator, web/sim: the same author)
// The panel's geometry (no DOM): the FM-1's own landscape layout and the stacked portrait one for phones, and
// the blend between them. app.js applies them.
import { B, KEYS } from './controls.js';

// ---- layouts: design-size boxes [x, y, w, h]; the device is scaled to fit the window
const WHITE = KEYS.filter(k => !k.black), BLACK = KEYS.filter(k => k.black);
function keybed(x0, y0, width, wW, wH, bW, bH, gapY) {    // whites in a row, the raised keys over their gaps
  const step = width / WHITE.length, out = {};
  WHITE.forEach((k, i) => { out[`key:${k.n}`] = [x0 + i * step + (step - wW) / 2, y0 + bH + gapY, wW, wH]; });
  BLACK.forEach(k => {
    const left = WHITE.findIndex(w => w.n === k.n - 1);                  // the white key below it
    out[`key:${k.n}`] = [x0 + (left + 1) * step - bW / 2, y0, bW, bH];
  });
  return out;
}
const ROWS = [['FX', 'SEL', 'ENV', 'LFO', 'EDIT', 'GLO'], ['HOME', 'SAVE', 'ARP', 'SEQ', 'PLAY', 'REC']];
function buttonRows(rows, x0, y0, size, step, rowStep) {
  const out = {};
  rows.forEach((r, j) => r.forEach((l, i) => { out[`btn:${B[l]}`] = [x0 + i * step, y0 + j * rowStep, size, size]; }));
  return out;
}
const knob = (cx, cy, d = 46) => [cx - d / 2, cy - d / 2, d, d];

export const DECO_CLASS = { bezel: 'bezel', oct: 'tray', buttons: 'tray', keys: 'tray keybed' };
export const LAYOUTS = {
  landscape: {                                     // the FM-1 as it is
    W: 1000, H: 620,
    deco: { bezel: [270, 36, 268, 268], oct: [52, 262, 196, 64], buttons: [572, 158, 394, 156], keys: [40, 336, 920, 262] },
    lcd: [284, 50, 240, 240],
    power: [478, -12, 56, 24],                      // the power switch: on the top edge, above the screen's right corner
    enc: { master: knob(95, 108), 0: knob(195, 108), 2: knob(95, 214), 1: knob(195, 214),
           3: knob(632, 108), 4: knob(731, 108), 5: knob(830, 108), 6: knob(929, 108) },
    ctl: { ...buttonRows(ROWS, 588, 174, 52, 62, 68),
           [`btn:${B['OCT-']}`]: [66, 275, 76, 38], [`btn:${B['OCT+']}`]: [158, 275, 76, 38],
           ...keybed(60, 350, 880, 47, 122, 44, 100, 14) },
  },
  portrait: {                                      // phones: the screen on top, OCT- / OCT+ end the button rows
    W: 420, H: 772,
    deco: { bezel: [76, 20, 268, 268], buttons: [14, 450, 392, 128], keys: [10, 590, 400, 172] },
    lcd: [90, 34, 240, 240],
    power: [284, -12, 56, 24],
    enc: { master: knob(60, 340), 0: knob(160, 340), 1: knob(260, 340), 2: knob(360, 340),
           3: knob(60, 416), 4: knob(160, 416), 5: knob(260, 416), 6: knob(360, 416) },
    ctl: { ...buttonRows([[...ROWS[0], 'OCT-'], [...ROWS[1], 'OCT+']], 25, 462, 46, 54, 58),
           ...keybed(16, 598, 388, 20, 76, 20, 72, 6) },
  },
};

const mix = (a, b, t) => a + (b - a) * t;
const clamp01 = x => Math.min(1, Math.max(0, x));
const ease = x => { x = clamp01(x); return x * x * (3 - 2 * x); };
const box = (a, b, t) => (t === 0 ? a : t === 1 ? b : a.map((v, i) => mix(v, b[i], t)));
const boxes = (a, b, t) => Object.fromEntries(Object.keys(a).map(k => [k, box(a[k], b[k], t)]));

// A at t = 0 .. B at t = 1, staggered: the body, its trays and the screen move first, then the knobs, the buttons,
// and the keys last, each part over 70 % of the morph (eased), so parts do not all cross at once. A tray B has not
// (the phone has no OCT tray) merges into B's button tray and fades.
const LAG = { body: 0, enc: 0.1, btn: 0.2, key: 0.3 };
const lag = (t, d) => ease((t - d) / 0.7);
export function blend(A, Bl, t) {
  const deco = {}, tb = lag(t, LAG.body);
  for (const [name, a] of Object.entries(A.deco))
    deco[name] = Bl.deco[name] ? { box: box(a, Bl.deco[name], tb), opacity: 1 }
      : { box: box(a, Bl.deco.buttons, tb), opacity: Math.max(0, 1 - t * 2.5) };
  const ctl = {};
  for (const [k, a] of Object.entries(A.ctl)) ctl[k] = box(a, Bl.ctl[k], lag(t, k.startsWith('key') ? LAG.key : LAG.btn));
  return { W: mix(A.W, Bl.W, tb), H: mix(A.H, Bl.H, tb), deco, lcd: box(A.lcd, Bl.lcd, tb), power: box(A.power, Bl.power, tb),
           enc: boxes(A.enc, Bl.enc, lag(t, LAG.enc)), ctl };
}
