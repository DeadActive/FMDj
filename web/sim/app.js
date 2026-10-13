// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE (the panel, the input and the power-on after Hortator's simulator, web/sim/app.js)
// The LSDj simulator page: draws the FM-1's panel in the Game Boy's colours (layout.js: the FM-1's own landscape, a
// stacked portrait for phones), turns pointer and keyboard input into messages for the worklet (worklet.js runs the
// firmware), paints the frames it sends back in the Game Boy screen's greens, and keeps what the firmware writes
// to flash in IndexedDB (store.js). The user's own ROM is made into a cartridge block here (lsdjinstall.js, as the
// install page does) and kept in this browser.
import { BTN, B, ENC, KEYS, GB, HeldSet, keyEvent, keyHint, contextMenuLatches } from './controls.js';
import { FlashStore, openFlashDb, loadCart, saveCart } from './store.js';
import { needsResume, playbackSession } from './audio.js';
import { LAYOUTS, DECO_CLASS, blend } from './layout.js';
import { loadEmu, makeCart } from './lsdjinstall.js';
import { songsPanel } from './songs.js';

const $ = id => document.getElementById(id);
const device = $('device'), fit = $('fit'), note = $('note');
const els = { deco: {}, ctl: {}, enc: {}, lbl: {} };
const lcd = document.createElement('canvas');
lcd.width = lcd.height = 240;
lcd.className = 'lcd';
lcd.setAttribute('aria-label', 'screen');
const lcdCtx = lcd.getContext('2d');

// ---- the Game Boy screen's greens: a grey of the firmware's picture -> a place between the darkest and the lightest
const RAMP = [[0x0f, 0x38, 0x0f], [0x30, 0x62, 0x30], [0x8b, 0xac, 0x0f], [0x9b, 0xbc, 0x0f]];
function green(l) {                                // l: 0 (black) .. 255 (white) -> 0xAABBGGRR
  const t = l / 255 * 3, i = Math.min(2, Math.floor(t)), f = t - i, a = RAMP[i], b = RAMP[i + 1];
  const c = k => Math.round(a[k] + (b[k] - a[k]) * f);
  return (0xff000000 | (c(2) << 16) | (c(1) << 8) | c(0)) >>> 0;
}
const LUT = new Uint32Array(65536);                // RGB565 -> the green for its brightness
for (let c = 0; c < 65536; c++) {
  const r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
  LUT[c] = green(0.299 * r + 0.587 * g + 0.114 * b);
}
const SHADE = [232, 160, 88, 16].map(green);       // the Game Boy's four shades, as the firmware's palette has them

function place(el, [x, y, w, h]) {
  el.style.left = `${x}px`; el.style.top = `${y}px`; el.style.width = `${w}px`; el.style.height = `${h}px`;
}
function span(cls, text) {
  const s = document.createElement('span');
  s.className = cls;
  s.textContent = text;
  return s;
}

// ---- the panel's elements (built once, placed per layout)
const DIR = new Set([B.HOME, B.ARP, B.SEL, B.SAVE]), PILL = new Set([B.SEQ, B.PLAY]);
function build() {
  for (const name of Object.keys(DECO_CLASS)) {
    const d = document.createElement('div');
    d.className = DECO_CLASS[name];
    device.append(d);
    els.deco[name] = d;
  }
  els.deco.bezel.append(span('stripes', ''), span('print', 'LSDJ · FM-1'), span('lamp', ''), span('lamp-print', 'POWER'));
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
    el.className = `key${k.black ? ' black' : ''}${!GB[ctl] ? ' idle' : GB[ctl].length === 1 ? ' ab' : ' page'}`;
    el.dataset.ctl = ctl;
    el.setAttribute('role', 'button');
    el.setAttribute('aria-label', GB[ctl] ? `${k.name}: ${GB[ctl]}` : k.name);
    if (GB[ctl]) el.append(span('', GB[ctl]));
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
}
function addHint(el, ctl) {
  if (keyHint[ctl]) el.append(span('hint', keyHint[ctl]));
}

let phone = false;
function relayout() {
  phone = window.innerWidth < 700 && window.innerHeight > window.innerWidth;
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
  const avail = Math.min(document.documentElement.clientWidth - 32, 1040);
  const scale = Math.max(0.2, Math.min(avail / G.W, phone ? 1.2 : 1));
  device.classList.toggle('phone', phone);
  device.style.width = `${G.W}px`;
  device.style.height = `${G.H}px`;
  device.style.setProperty('--scale', scale);
  fit.style.width = `${G.W * scale}px`;
  fit.style.height = `${G.H * scale}px`;
}

// ---- input -> worklet
let node = null;
const held = new HeldSet();
function send(m) { if (node) node.port.postMessage(m); }
function ctlMsg(ctl, down) {
  const [kind, id] = ctl.split(':');
  return { t: kind, id: Number(id), down };
}
function press(ctl, src) {
  if (held.press(ctl, src)) { send(ctlMsg(ctl, true)); els.ctl[ctl]?.classList.add('down'); }
}
function release(ctl, src) {
  if (held.release(ctl, src)) { send(ctlMsg(ctl, false)); els.ctl[ctl]?.classList.remove('down'); }
  if (src === 'latch') els.ctl[ctl]?.classList.remove('latched');
}
function releaseAll() {
  for (const ctl of held.releaseAll()) {
    send(ctlMsg(ctl, false));
    els.ctl[ctl]?.classList.remove('down', 'latched');
  }
  send({ t: 'release' });
}
function toggleLatch(ctl) {
  if (held.holds(ctl, 'latch')) release(ctl, 'latch');
  else { press(ctl, 'latch'); els.ctl[ctl].classList.add('latched'); }
}

function bindControls() {
  let lastPointerType;
  device.addEventListener('contextmenu', e => {          // right-click a button: hold it (combinations)
    const el = e.target.closest('[data-ctl]');
    e.preventDefault();
    if (el && el.classList.contains('btn') && contextMenuLatches(lastPointerType)) toggleLatch(el.dataset.ctl);
  });
  device.addEventListener('pointerdown', e => {
    const el = e.target.closest('[data-ctl]');
    lastPointerType = e.pointerType;
    if (!el || e.button !== 0) return;
    const ctl = el.dataset.ctl, src = `p${e.pointerId}`;
    e.preventDefault();
    el.setPointerCapture(e.pointerId);
    if (held.holds(ctl, 'latch')) { release(ctl, 'latch'); return; }   // a tap lets a held button go
    press(ctl, src);
    if (el.classList.contains('btn') && e.pointerType !== 'mouse') {
      const timer = setTimeout(() => { if (held.holds(ctl, src)) toggleLatch(ctl); }, 600);
      el.addEventListener('pointerup', () => clearTimeout(timer), { once: true });
    }
  });
  const up = e => {
    const el = e.target.closest?.('[data-ctl]');
    if (el) release(el.dataset.ctl, `p${e.pointerId}`);
  };
  device.addEventListener('pointerup', up);
  device.addEventListener('pointercancel', up);
  device.addEventListener('lostpointercapture', up);
  for (const [id, el] of Object.entries(els.enc)) bindEncoder(id, el);
  window.addEventListener('keydown', e => {
    if (!node || e.target.closest?.('input, a, button.link')) return;   // (the power switch keeps no key: Enter is START)
    const k = keyEvent(e);
    if (k) { e.preventDefault(); press(k.ctl, 'kbd'); }
  });
  window.addEventListener('keyup', e => {
    const k = keyEvent(e);
    if (k) { e.preventDefault(); release(k.ctl, 'kbd'); }
  });
  window.addEventListener('blur', releaseAll);
  document.addEventListener('visibilitychange', () => { if (document.hidden) releaseAll(); else wake(); });
  device.addEventListener('pointerdown', wake, true);
  window.addEventListener('keydown', wake, true);
}

// knobs: 12 px of drag = one step, up = clockwise; MASTER is a pot (0..1023 over 270 degrees)
let masterValue = 800;
const angle = {};
function turn(id, steps) {
  if (id === 'master') {
    masterValue = Math.max(0, Math.min(1023, masterValue + steps * 32));
    showMaster();
    send({ t: 'master', value: masterValue });
    return;
  }
  angle[id] = (angle[id] ?? 0) + steps * 15;
  els.enc[id].style.setProperty('--turn', `${angle[id]}deg`);
  send({ t: 'enc', id: Number(id), steps });
}
function showMaster() { els.enc.master.style.setProperty('--turn', `${-135 + masterValue / 1023 * 270}deg`); }
function bindEncoder(id, el) {
  let acc = 0, lastY = 0, wheel = 0;
  el.addEventListener('pointerdown', e => {
    e.preventDefault();
    el.setPointerCapture(e.pointerId);
    lastY = e.clientY;
    acc = 0;
  });
  el.addEventListener('pointermove', e => {
    if (!el.hasPointerCapture(e.pointerId)) return;
    const scale = parseFloat(getComputedStyle(device).getPropertyValue('--scale')) || 1;
    acc += (lastY - e.clientY) / scale;
    lastY = e.clientY;
    const px = id === 'master' ? 4 : 12, steps = Math.trunc(acc / px);
    if (steps) { acc -= steps * px; turn(id, id === 'master' ? steps / 8 : steps); }
  });
  el.addEventListener('wheel', e => {
    e.preventDefault();
    wheel -= e.deltaY;
    const steps = Math.trunc(wheel / 40);
    if (steps) { wheel -= steps * 40; turn(id, steps); }
  }, { passive: false });
}

// ---- frames from the worklet
let frame = null;
const img = lcdCtx.createImageData(240, 240);
const px = new Uint32Array(img.data.buffer);
function blank() {                                 // the screen with nothing on it: its lightest green
  px.fill(SHADE[0]);
  lcdCtx.putImageData(img, 0, 0);
}
function paint() {
  if (frame) {
    const fb = frame.fb;
    frame = null;
    for (let i = 0; i < fb.length; i++) px[i] = LUT[fb[i]];
    lcdCtx.putImageData(img, 0, 0);
  }
  requestAnimationFrame(paint);
}
// the Game Boy's own picture (160 x 144 shades), 1.5x, while the ROM is checked before power-on
const small = document.createElement('canvas');
small.width = 160; small.height = 144;
const smallCtx = small.getContext('2d'), smallImg = smallCtx.createImageData(160, 144), smallPx = new Uint32Array(smallImg.data.buffer);
function preview(shades) {
  for (let i = 0; i < shades.length; i++) smallPx[i] = SHADE[shades[i] & 3];
  smallCtx.putImageData(smallImg, 0, 0);
  lcdCtx.imageSmoothingEnabled = false;
  lcdCtx.drawImage(small, 0, 4, 240, 216);
}

// ---- the ROM: made into a cartridge block once (lsdjinstall.js), kept in this browser
const fetchBytes = async name => {
  const r = await fetch(name);
  if (!r.ok) throw new Error(`${name}: HTTP ${r.status}`);
  return r.arrayBuffer();
};
const wasmBytes = fetchBytes('lsdjsim.wasm');
let cart = null, busy = false;
function ready(text) {
  $('rom-line').textContent = text;
  $('rom-pick').hidden = !!cart;
  $('forget-rom').hidden = !cart;
  $('forget-save').hidden = !cart;
  els.power.disabled = !cart || busy;
  els.power.classList.toggle('ready', !!cart && !node && !busy);
}
async function chooseRom(file) {
  if (!file || busy) return;
  busy = true;
  $('rom').disabled = true;
  $('rom-bar').hidden = false;
  $('rom-bar').value = 0;
  try {
    const [emuWasm, demo] = await Promise.all([fetchBytes('lsdj_emu.wasm'),
      fetch('demo.json').then(r => (r.ok ? r.json() : null), () => null)]);
    const emu = await loadEmu(emuWasm), parts = { "LSDj's first start": [0, 8], 'typing the demo song': [8, 90], 'playing it': [98, 2] };
    const made = await makeCart(emu, new Uint8Array(await file.arrayBuffer()), demo, 1 << 20, (what, done, total) => {
      const [at, share] = parts[what] ?? [0, 100];
      $('rom-bar').value = at + share * done / total;
      $('rom-line').textContent = `Checking the ROM: ${what} …`;
      preview(emu.screen());
    }, () => new Promise(r => setTimeout(r)));
    const kept = await saveCart(made);
    cart = made;
    busy = false;
    blank();
    ready(`${file.name} runs. ${kept ? 'It is kept in this browser.' : 'This browser blocks storage, so it is asked for again next time.'} Switch the power on.`);
  } catch (e) {
    busy = false;
    blank();
    ready(e.message);
  }
  $('rom-bar').hidden = true;
  $('rom').disabled = false;
  $('rom').value = '';
}

// ---- the firmware's console, for songs and saves (songs.js): a line to the worklet, its output back
const asked = new Map();
let askId = 0, songs = null, started = false;
function ask(line) {
  return new Promise(resolve => {
    asked.set(++askId, resolve);
    send({ t: 'con', id: askId, line });
  });
}

// ---- flash in IndexedDB (store.js), the audio clock (audio.js)
const store = new FlashStore(openFlashDb, msg => { note.textContent = msg; });
let ctx = null, poweredOff = false;
function wake() {                                 // a tap or a key brings a stopped context back (not one switched off)
  if (!ctx || poweredOff || !needsResume(ctx.state)) return;
  ctx.resume().catch(() => {});
}
function setSwitch(on) {
  els.power.setAttribute('aria-checked', String(on));
  els.power.classList.toggle('on', on);
  device.classList.toggle('on', on);
  device.classList.toggle('off', !on && !!node);
}
async function powerSwitch() {                     // the first flip starts the firmware; later ones pause / resume
  els.power.blur();
  if (!cart || busy) return;
  if (!node) return powerOn();
  poweredOff = !poweredOff;
  setSwitch(!poweredOff);
  if (poweredOff) { releaseAll(); await ctx.suspend(); }
  else await ctx.resume();
}
async function powerOn() {
  const btn = els.power;
  btn.disabled = true;
  btn.classList.remove('ready');
  setSwitch(true);
  try {
    if (!window.AudioWorkletNode) throw new Error('This browser has no AudioWorklet. Use a current Chrome, Firefox or Safari.');
    playbackSession(navigator);
    ctx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
    const resumed = ctx.resume();                         // inside the tap: browsers allow sound from here on
    await ctx.audioWorklet.addModule('worklet.js');
    const [wasm, sectors] = await Promise.all([wasmBytes, store.load()]);
    const n = new AudioWorkletNode(ctx, 'fm1', { numberOfInputs: 0, outputChannelCount: [2], processorOptions: { wasm, sectors, cart } });
    n.port.onmessage = e => {
      const m = e.data;
      if (m.t === 'frame') {
        frame = m;
        if (m.playing && !started) { started = true; songs?.refresh(); }   // PLAY has started LSDj: its songs
      }
      else if (m.t === 'flash') store.put(m.sectors);
      else if (m.t === 'con') { asked.get(m.id)?.(m.text); asked.delete(m.id); }
      else if (m.t === 'error') note.textContent = `The firmware failed to start: ${m.message}`;
    };
    n.connect(ctx.destination);
    await resumed;
    node = n;
    send({ t: 'master', value: masterValue });
    $('songs').hidden = false;
    songs = songsPanel($('songs'), ask, () => !!node && !poweredOff && ctx.state === 'running');
    songs.refresh();
    $('rom-line').textContent = 'Press PLAY (Enter) to start LSDj, and PLAY again to play the song.';
    $('forget-rom').hidden = $('forget-save').hidden = false;
    if (ctx.sampleRate !== 44100) note.textContent = `Audio runs at ${ctx.sampleRate} Hz here, not 44100 Hz: pitch and tempo are off.`;
    ctx.addEventListener('statechange', () => {
      if (needsResume(ctx.state) && !document.hidden && !poweredOff) note.textContent = 'Sound stopped. Click the panel to start it again.';
      else if (ctx.state === 'running' && note.textContent.startsWith('Sound stopped')) note.textContent = '';
    });
  } catch (err) {
    setSwitch(false);
    note.textContent = err.message;
  }
  btn.disabled = false;
}

// ---- start
build();
relayout();
blank();
showMaster();
bindControls();
window.addEventListener('resize', relayout);
els.power.addEventListener('click', powerSwitch);
$('rom').addEventListener('change', () => chooseRom($('rom').files[0]));
$('forget-rom').addEventListener('click', async () => { await saveCart(null); location.reload(); });
$('forget-save').addEventListener('click', async () => {
  if (!confirm('Forget the song saved in this browser? LSDj starts from the demo song again.')) return;
  await store.clear();
  location.reload();
});
$('hints').addEventListener('change', () => device.classList.toggle('show-hints', $('hints').checked));
device.classList.add('show-hints');
requestAnimationFrame(paint);
loadCart().then(c => {
  cart = c;
  ready(c ? 'Your LSDj ROM is kept in this browser. Switch the power on.' : 'To begin, choose your own LSDj ROM file.');
});
