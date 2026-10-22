// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE (the panel, the input and the power-on after Hortator's simulator, web/sim/app.js)
// The LSDj simulator page: draws the FM-1's panel in the Game Boy's colours (panel.js, placed by layout.js: the FM-1's
// own landscape, a stacked portrait for phones), turns pointer and keyboard input into messages for the worklet
// (worklet.js runs the firmware), paints the frames it sends back in their own colours (the firmware's screen theme), and keeps what the
// firmware writes to flash in IndexedDB (store.js). The user's own ROM is made into a cartridge block here
// (lsdjinstall.js, as the install page does) and kept in this browser.
import { HeldSet, keyEvent, contextMenuLatches, setProgram } from './controls.js';
import { FlashStore, openFlashDb, loadCart, saveCart } from './store.js';
import { needsResume, playbackSession } from './audio.js';
import { buildPanel, layoutPanel, LUT, SHADE } from './panel.js';
import { loadEmu, makeCart, cartProg } from './lsdjinstall.js';
import { songsPanel } from './songs.js';

const $ = id => document.getElementById(id);
const device = $('device'), fit = $('fit'), note = $('note');
const lcd = document.createElement('canvas');
lcd.width = lcd.height = 240;
lcd.className = 'lcd';
lcd.setAttribute('aria-label', 'screen');
const lcdCtx = lcd.getContext('2d');
let els;                                           // the panel's elements (panel.js), built at the start

const relayout = () => layoutPanel(device, fit, els, lcd, Math.min(document.documentElement.clientWidth - 32, 1040));

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
    const emu = await loadEmu(emuWasm), parts = { "LSDj's first start": [0, 8], 'typing the demo song': [8, 90], 'playing it': [98, 2],
      "DMGo's first start": [0, 40], 'playing a step': [40, 60] };
    const made = await makeCart(emu, new Uint8Array(await file.arrayBuffer()), demo, 1 << 20, (what, done, total) => {
      const [at, share] = parts[what] ?? [0, 100];
      $('rom-bar').value = at + share * done / total;
      $('rom-line').textContent = `Checking the ROM: ${what} …`;
      preview(emu.screen());
    }, () => new Promise(r => setTimeout(r)));
    const kept = await saveCart(made);
    if (kept && cartProg(made) !== (program ?? 'lsdj')) { location.reload(); return; }   // DMGo's keys on the panel
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
    $('rom-line').textContent = program === 'dmgo' ?
      'Press PLAY (Enter) to start DMGo: the white keys are its steps, A is D#5 (X), B is F#5 (Z).' :
      'Press PLAY (Enter) to start LSDj, and PLAY again to play the song.';
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

// ---- start: the kept cartridge first, as its program decides what the keys are (LSDj's or DMGo's)
const kept0 = await loadCart().catch(() => null), program = cartProg(kept0);
const NAME = { lsdj: 'LSDj', dmgo: 'DMGo' };
setProgram(program);
els = buildPanel(device, lcd);
relayout();
blank();
showMaster();
bindControls();
window.addEventListener('resize', relayout);
els.power.addEventListener('click', powerSwitch);
$('rom').addEventListener('change', () => chooseRom($('rom').files[0]));
$('forget-rom').addEventListener('click', async () => { await saveCart(null); location.reload(); });
$('forget-save').addEventListener('click', async () => {
  if (!confirm(program === 'dmgo' ? 'Forget the save kept in this browser? DMGo starts empty again.' :
    'Forget the song saved in this browser? LSDj starts from the demo song again.')) return;
  await store.clear();
  location.reload();
});
$('hints').addEventListener('change', () => device.classList.toggle('show-hints', $('hints').checked));
device.classList.add('show-hints');
requestAnimationFrame(paint);
cart = kept0;
ready(cart ? `Your ${NAME[program] || 'ROM'} is kept in this browser. Switch the power on.` :
  'To begin, choose your own LSDj or DMGo ROM file.');
