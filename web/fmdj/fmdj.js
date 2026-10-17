// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
// The FMDj landing page: the FM-1's panel as the simulator draws it (webapp/lsdj-sim/panel.js), a picture only (no
// sound, no input), with LSDj's screens in its screen. The screens are the gallery's pictures (web/make_site.py puts
// them on the page from shots/manifest.json; none, no gallery): each is LSDj's 160 x 144 picture at 3x in the Game
// Boy's greens, drawn here as the firmware shows it, at 1.5x in rows 4 .. 219 of the 240 x 240 screen. The next one
// every 3 s; a click on one shows it and keeps it.
import { buildPanel, layoutPanel, SHADE } from './webapp/lsdj-sim/panel.js';

const $ = id => document.getElementById(id);
const device = $('device'), fit = $('fit');
const lcd = document.createElement('canvas');
lcd.width = lcd.height = 240;
lcd.className = 'lcd';
lcd.setAttribute('aria-label', 'screen');
const lcdCtx = lcd.getContext('2d');
const els = buildPanel(device, lcd);
device.inert = true;                               // a picture of the FM-1: nothing on it takes focus or input
const relayout = () => layoutPanel(device, fit, els, lcd, Math.min(document.documentElement.clientWidth - 32, 1040));
relayout();
window.addEventListener('resize', relayout);

// the screen with nothing on it: its lightest green
const img = lcdCtx.createImageData(240, 240);
new Uint32Array(img.data.buffer).fill(SHADE[0]);
lcdCtx.putImageData(img, 0, 0);

// a screenshot (480 x 432, the tool's four greens) -> LSDj's 160 x 144 picture in the firmware's four shades, 1.5x
const small = document.createElement('canvas');
small.width = 160; small.height = 144;
const smallCtx = small.getContext('2d', { willReadFrequently: true });
function draw(pic) {
  smallCtx.imageSmoothingEnabled = false;
  smallCtx.drawImage(pic, 0, 0, 160, 144);
  const d = smallCtx.getImageData(0, 0, 160, 144), px = new Uint32Array(d.data.buffer);
  for (let i = 0; i < px.length; i++) {             // the green channel tells the shade: BC, AC, 62, 38
    const g = (px[i] >>> 8) & 0xff;
    px[i] = SHADE[g >= 0xb4 ? 0 : g >= 0x87 ? 1 : g >= 0x4d ? 2 : 3];
  }
  smallCtx.putImageData(d, 0, 0);
  lcdCtx.imageSmoothingEnabled = false;
  lcdCtx.drawImage(small, 0, 4, 240, 216);
}

const shots = [...document.querySelectorAll('#shots button')];
let at = -1, timer = 0;
async function show(i) {
  const b = shots[i], pic = b.querySelector('img');
  at = i;
  shots.forEach((s, k) => s.setAttribute('aria-pressed', String(k === i)));
  try { await pic.decode(); } catch { return; }    // a picture that does not load leaves the screen as it was
  if (at !== i) return;
  draw(pic);
  lcd.setAttribute('aria-label', `screen: LSDj's ${b.dataset.title} screen`);
}
if (shots.length) {
  device.classList.add('on');                      // the power lamp lit, the switch on
  els.power.classList.add('on');
  els.power.setAttribute('aria-checked', 'true');
  shots.forEach((b, i) => b.addEventListener('click', () => { clearInterval(timer); show(i); }));
  show(0);
  if (!matchMedia('(prefers-reduced-motion: reduce)').matches)
    timer = setInterval(() => show((at + 1) % shots.length), 3000);
}
