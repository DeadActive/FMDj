// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE (from Hortator's simulator, web/sim: the same author)
// The FM-1 panel's controls as data (no DOM): label ids are panel.c's enum order (tests/lsdj/lsdj_sim_test.mjs checks
// it), the 27 keys, what each control is to the Game Boy (docs/lsdj/CONTROLS.md), the computer keyboard map, and the
// held-state bookkeeping the page uses.
export const BTN = [
  { c: 'B_FX', label: 'FX' }, { c: 'B_SCL', label: 'SEL' }, { c: 'B_ENV', label: 'ENV' }, { c: 'B_LFO', label: 'LFO' },
  { c: 'B_EDIT', label: 'EDIT' }, { c: 'B_GLO', label: 'GLO' }, { c: 'B_HOME', label: 'HOME' },
  { c: 'B_SAVE', label: 'SAVE' }, { c: 'B_ARP', label: 'ARP' }, { c: 'B_SEQ', label: 'SEQ' },
  { c: 'B_PLAY', label: 'PLAY' }, { c: 'B_REC', label: 'REC' }, { c: 'B_OCTDN', label: 'OCT-' }, { c: 'B_OCTUP', label: 'OCT+' },
];
export const B = Object.fromEntries(BTN.map((b, i) => [b.label, i]));

export const ENC = [
  { c: 'EN_SELECT', label: 'SELECT' }, { c: 'EN_ALGO', label: 'ALGORITHM' }, { c: 'EN_PRESET', label: 'PRESETS' },
  { c: 'EN_K1', label: 'KNOB1' }, { c: 'EN_K2', label: 'KNOB2' }, { c: 'EN_K3', label: 'KNOB3' }, { c: 'EN_K4', label: 'KNOB4' },
];

const NAMES = ['F', 'F#', 'G', 'G#', 'A', 'A#', 'B', 'C', 'C#', 'D', 'D#', 'E'];
export const KEYS = Array.from({ length: 27 }, (_, n) => {
  const name = NAMES[n % 12], octave = 3 + Math.floor((n + 5) / 12);   // F3 .. G5 (C starts an octave)
  return { n, name: name + octave, black: name.includes('#') };
});

// what each control is in the LSDj build (firmware/src/lsdj/lsdj.c KEYS, ble_key): 'btn:<label id>', 'key:<n>',
// 'enc:<role>' -> the Game Boy's key, or the FM-1's own function
export const GB = {
  [`btn:${B.SEQ}`]: 'SELECT', [`btn:${B.PLAY}`]: 'START', [`btn:${B.HOME}`]: '\u25C0', [`btn:${B.ARP}`]: '\u25B6',
  [`btn:${B.SEL}`]: '\u25B2', [`btn:${B.SAVE}`]: '\u25BC', [`btn:${B.REC}`]: 'SAVE', [`btn:${B['OCT+']}`]: 'SEL+\u25B2',
  [`btn:${B['OCT-']}`]: 'SEL+\u25BC', [`btn:${B.FX}`]: 'DRAW', [`btn:${B.LFO}`]: 'ROM', [`btn:${B.EDIT}`]: 'WAIT',
  [`btn:${B.ENV}`]: 'SOUND', 'key:24': 'A', 'key:26': 'B',
  'enc:0': '\u25C0 \u25B6', 'enc:1': '\u25B2 \u25BC', 'enc:2': 'SEL + \u25C0 \u25B6', 'enc:master': 'VOLUME',
};

// KeyboardEvent.code (the physical key, any layout) -> control, as Game Boy emulators have it: the arrows, X = A,
// Z = B, Enter = START, Shift = SELECT (so Shift + an arrow changes LSDj's screen, X + an arrow a value). S held: save.
export const KEYBOARD = {
  ArrowLeft: { kind: 'btn', id: B.HOME }, ArrowRight: { kind: 'btn', id: B.ARP },
  ArrowUp: { kind: 'btn', id: B.SEL }, ArrowDown: { kind: 'btn', id: B.SAVE },
  KeyX: { kind: 'key', id: 24 }, KeyZ: { kind: 'key', id: 26 },
  Enter: { kind: 'btn', id: B.PLAY }, ShiftLeft: { kind: 'btn', id: B.SEQ }, ShiftRight: { kind: 'btn', id: B.SEQ },
  KeyS: { kind: 'btn', id: B.REC },
};
const SHOWN = { ArrowLeft: '\u2190', ArrowRight: '\u2192', ArrowUp: '\u2191', ArrowDown: '\u2193', KeyX: 'X', KeyZ: 'Z',
  Enter: 'Enter', ShiftLeft: 'Shift', KeyS: 'S' };
export const keyHint = Object.fromEntries(Object.entries(KEYBOARD).filter(([code]) => SHOWN[code])
  .map(([code, c]) => [`${c.kind}:${c.id}`, SHOWN[code]]));

// a keyboard event -> { ctl: 'key:n' | 'btn:id', down } or null (unmapped, autorepeat, a shortcut with a modifier)
export function keyEvent(e) {
  const c = KEYBOARD[e.code];
  const down = e.type === 'keydown';
  if (!c || e.repeat || (down && (e.metaKey || e.ctrlKey || e.altKey))) return null;
  return { ctl: `${c.kind}:${c.id}`, down };
}

// does a contextmenu event latch a button? A right-click does; a touch / pen long-press fires contextmenu too (Android
// Chrome, Windows) and the long-press timer latches it, so that one must not (it would toggle the latch back off)
export function contextMenuLatches(lastPointerType) { return lastPointerType !== 'touch' && lastPointerType !== 'pen'; }

// a control is held while any source (a pointer, the keyboard, a latch) holds it
export class HeldSet {
  constructor() { this.m = new Map(); }
  press(ctl, src) {                                // -> true when the control just went down
    const s = this.m.get(ctl) ?? new Set();
    const first = s.size === 0;
    s.add(src);
    this.m.set(ctl, s);
    return first;
  }
  release(ctl, src) {                              // -> true when the control just went up
    const s = this.m.get(ctl);
    if (!s || !s.delete(src)) return false;
    if (s.size) return false;
    this.m.delete(ctl);
    return true;
  }
  holds(ctl, src) { return this.m.get(ctl)?.has(src) ?? false; }
  releaseAll() {                                   // -> the controls that were held
    const all = [...this.m.keys()];
    this.m.clear();
    return all;
  }
}
