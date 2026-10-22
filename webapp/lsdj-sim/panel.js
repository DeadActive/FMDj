// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE (the panel after Hortator's simulator, web/sim/app.js)
// The FM-1's panel in the Game Boy's colours, as elements: built once (buildPanel), placed per layout (layoutPanel:
// layout.js's FM-1 landscape, the stacked portrait for phones), and the Game Boy screen's greens. The simulator page
// (app.js) and the FMDj landing page (web/fmdj/fmdj.js) share it.
import { BTN, B, ENC, KEYS, GB, STEP, keyHint } from './controls.js';
import { LAYOUTS, DECO_CLASS, blend } from './layout.js';

// ---- the Game Boy screen's greens: a grey of the firmware's picture -> a place between the darkest and the lightest
const RAMP = [[0x0f, 0x38, 0x0f], [0x30, 0x62, 0x30], [0x8b, 0xac, 0x0f], [0x9b, 0xbc, 0x0f]];
export function green(l) {                         // l: 0 (black) .. 255 (white) -> 0xAABBGGRR
  const t = l / 255 * 3, i = Math.min(2, Math.floor(t)), f = t - i, a = RAMP[i], b = RAMP[i + 1];
  const c = k => Math.round(a[k] + (b[k] - a[k]) * f);
  return (0xff000000 | (c(2) << 16) | (c(1) << 8) | c(0)) >>> 0;
}
// RGB565 -> its own colour, as the FM-1's screen shows it: the Game Boy's colours are the firmware's own now (the GLO
// menu's SCREEN themes; DMG green at first)
export const LUT = new Uint32Array(65536);
for (let c = 0; c < 65536; c++) {
  const r = Math.round(((c >> 11) & 31) * 255 / 31), g = Math.round(((c >> 5) & 63) * 255 / 63), b = Math.round((c & 31) * 255 / 31);
  LUT[c] = (0xff000000 | (b << 16) | (g << 8) | r) >>> 0;
}
export const SHADE = [232, 160, 88, 16].map(green); // the Game Boy's four shades, as the firmware's palette has them

function place(el, [x, y, w, h]) {
  el.style.left = `${x}px`; el.style.top = `${y}px`; el.style.width = `${w}px`; el.style.height = `${h}px`;
}
function span(cls, text) {
  const s = document.createElement('span');
  s.className = cls;
  s.textContent = text;
  return s;
}

// ---- the panel's elements (built once, placed per layout) -> { deco, ctl, enc, lbl, power, powerPrint }
const DIR = new Set([B.HOME, B.ARP, B.SEL, B.SAVE]), PILL = new Set([B.SEQ, B.PLAY]);
export function buildPanel(device, lcd) {
  const els = { deco: {}, ctl: {}, enc: {}, lbl: {} };
  for (const name of Object.keys(DECO_CLASS)) {
    const d = document.createElement('div');
    d.className = DECO_CLASS[name];
    device.append(d);
    els.deco[name] = d;
  }
  els.deco.bezel.append(span('stripes', ''), span('print', 'FMDJ · FM-1'), span('lamp', ''), span('lamp-print', 'POWER'));
  device.append(lcd);
  els.power = document.createElement('button');
  els.power.className = 'power';
  els.power.id = 'power-on';
  els.power.setAttribute('role', 'switch');
  els.power.setAttribute('aria-checked', 'false');
  els.power.setAttribute('aria-label', 'power');
  els.power.disabled = true;
  els.powerPrint = document.createElement('div');
  els.powerPrint.className = 'power-print';
  els.powerPrint.textContent = '◂ OFF · ON ▸';
  device.append(els.power, els.powerPrint);
  BTN.forEach((b, id) => {
    const el = document.createElement('div'), ctl = `btn:${id}`;
    el.className = `btn${DIR.has(id) ? ' dir' : ''}${PILL.has(id) ? ' pill' : ''}${GB[ctl] ? '' : ' idle'}`;
    el.dataset.ctl = ctl;
    el.setAttribute('role', 'button');
    el.setAttribute('aria-label', GB[ctl] ? `${b.label}: ${GB[ctl]}` : b.label);
    el.append(span('', b.label));
    if (GB[ctl]) el.append(span('gb', GB[ctl]));
    addHint(el, ctl);
    device.append(el);
    els.ctl[ctl] = el;
  });
  KEYS.forEach(k => {
    const el = document.createElement('div'), ctl = `key:${k.n}`;
    el.className = `key${k.black ? ' black' : ''}${GB[ctl] ? ' ab' : STEP[ctl] ? ' step' : ' idle'}`;
    el.dataset.ctl = ctl;
    el.setAttribute('role', 'button');
    el.setAttribute('aria-label', GB[ctl] ? `${k.name}: ${GB[ctl]}` : STEP[ctl] ? `${k.name}: step ${STEP[ctl]}` : k.name);
    if (GB[ctl] || STEP[ctl]) el.append(span('', GB[ctl] || STEP[ctl]));
    addHint(el, ctl);
    device.append(el);
    els.ctl[ctl] = el;
  });
  [...ENC.map((e, i) => [String(i), e.label]), ['master', 'MASTER']].forEach(([id, label]) => {
    const el = document.createElement('div'), gb = GB[`enc:${id}`];
    el.className = `enc${gb ? '' : ' idle'}`;
    el.tabIndex = gb ? 0 : -1;
    el.setAttribute('role', 'slider');
    el.setAttribute('aria-label', gb ? `${label}: ${gb}` : label);
    el.append(span('skirt', ''), span('cap', ''));
    device.append(el);
    els.enc[id] = el;
    const l = document.createElement('div');
    l.className = `lbl${gb ? '' : ' idle'}`;
    l.append(document.createTextNode(label));
    if (gb) l.append(span('gb', gb));
    device.append(l);
    els.lbl[id] = l;
  });
  return els;
}
function addHint(el, ctl) {
  if (keyHint[ctl]) el.append(span('hint', keyHint[ctl]));
}

// ---- the elements placed for the window (portrait phones get the stacked layout), scaled to availWidth -> phone
export function layoutPanel(device, fit, els, lcd, availWidth) {
  const phone = window.innerWidth < 700 && window.innerHeight > window.innerWidth;
  const A = LAYOUTS.landscape, G = blend(A, phone ? LAYOUTS.portrait : A, phone ? 1 : 0);
  for (const [name, d] of Object.entries(G.deco)) {
    const b = d.box;
    place(els.deco[name], name !== 'bezel' ? b : phone ? [b[0] - 8, b[1] - 8, b[2] + 16, b[3] + 16] : [b[0] - 22, b[1] - 14, b[2] + 44, b[3] + 28]);
    els.deco[name].style.opacity = d.opacity;
  }
  place(lcd, G.lcd);
  place(els.power, phone ? [14, 3, 50, 12] : [64, 2, 54, 13]);
  place(els.powerPrint, phone ? [70, 5, 80, 10] : [126, 4, 90, 10]);
  for (const [ctl, b] of Object.entries(G.ctl)) place(els.ctl[ctl], b);
  for (const [id, b] of Object.entries(G.enc)) {
    place(els.enc[id], b);
    place(els.lbl[id], [b[0] - 30, b[1] - (phone ? 15 : 30), b[2] + 60, 28]);
    els.lbl[id].querySelector('.gb')?.toggleAttribute('hidden', phone);
  }
  const scale = Math.max(0.2, Math.min(availWidth / G.W, phone ? 1.2 : 1));
  device.classList.toggle('phone', phone);
  device.style.width = `${G.W}px`;
  device.style.height = `${G.H}px`;
  device.style.setProperty('--scale', scale);
  fit.style.width = `${G.W * scale}px`;
  fit.style.height = `${G.H * scale}px`;
  return phone;
}
