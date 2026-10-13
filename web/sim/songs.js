// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
// Songs and saves on the simulator page: the song page's own flows (web/lsdjlink.js: the list, a project or song in,
// a song out, the whole save out and in, Save now) through simlink.js to the simulated firmware's console. Kits
// that a project brings go into the simulated flash's kit banks, as on an FM-1.
import { Link, listSongs, downloadSong, uploadSong, deleteSong, downloadSav, uploadSav, saveToFlash } from './lsdjlink.js';
import { simLink } from './simlink.js';

const here = text => text.replace(/on the FM-1|the FM-1's flash|the FM-1/g, m => (m === 'on the FM-1' ? 'here' : m === "the FM-1's flash" ? "this browser's storage" : 'the simulator'))
  .replace(/^FM-1: /, 'The firmware says: ');

function saveFile(name, bytes) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([bytes]));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
}
const stamp = () => new Date().toISOString().slice(0, 16).replace(/[-:]/g, '').replace('T', '-');

// root: the #songs section; ask: async (line) -> the firmware's console output; running: () -> is the power on
export function songsPanel(root, ask, running) {
  const link = simLink(Link, ask), $ = id => root.querySelector('#' + id);
  const line = $('songs-line'), bar = $('songs-bar'), body = root.querySelector('#songs-table tbody');
  let busy = false;
  const say = t => { line.textContent = t; };
  const progress = (done, total) => { bar.hidden = false; bar.value = total ? 100 * done / total : 0; };
  const kept = (what, r) => what + (r && r.unsaved ? ` It is NOT kept in this browser: ${here(r.unsaved)}.` : '');
  function lock(on) {
    busy = on;
    for (const el of root.querySelectorAll('button')) el.disabled = on;
    for (const el of root.querySelectorAll('label.act')) el.classList.toggle('off', on);
  }

  function draw(list) {
    body.textContent = '';
    for (const s of list) {
      const tr = document.createElement('tr');
      const cell = (text, cls) => { const td = document.createElement('td'); td.textContent = text; td.className = cls; tr.append(td); return td; };
      cell(String(s.slot + 1).padStart(2, '0'), 'small');
      cell(`${s.name}.${s.version.toString(16).toUpperCase()}`, 'name');
      cell(`${s.blocks} block${s.blocks > 1 ? 's' : ''}`, 'small');
      const act = cell('', 'do');
      const button = (text, fn) => { const b = document.createElement('button'); b.textContent = text; b.onclick = fn; act.append(b); };
      button('Download', () => job(async () => {
        const f = await downloadSong(link, s.slot, progress, s);
        saveFile(f.name, f.bytes);
        return `Downloaded ${f.name}.`;
      }));
      button('Delete', () => {
        if (!confirm(`Delete ${s.name} from LSDj's song list? LSDj starts again.`)) return;
        job(async () => kept(`Deleted ${s.name}.`, await deleteSong(link, s.slot, s)));
      });
      body.append(tr);
    }
  }

  async function refresh(said) {
    const inf = await link.info();
    if (!inf.started) { draw([]); say('Press PLAY (Enter) to start LSDj; its songs are listed then.'); return; }
    const l = await listSongs(link);
    draw(l.list);
    say(said || (l.list.length ? `${l.list.length} song${l.list.length > 1 ? 's' : ''} in LSDj's list; ${l.free} of 191 blocks free.`
      : "No song in LSDj's list yet. Load a project, or save the song you are editing in LSDj (PROJECT, LOAD/SAVE SONG, SAVE)."));
  }

  // one thing at a time; what went wrong is said in a sentence; the list is read again after
  async function job(fn) {
    if (busy) return;
    if (!running()) { say('Switch the power on first.'); return; }
    lock(true);
    let said = '';
    try {
      said = await fn();
    } catch (e) {
      said = /does not answer/.test(e.message) ? 'The simulator does not answer: is its power on?' : here(e.message);
    }
    bar.hidden = true;
    try { await refresh(said); } catch (e) { say(said || here(e.message)); }
    lock(false);
  }

  $('songs-refresh').onclick = () => job(async () => '');
  $('save-now').onclick = () => job(async () => { await saveToFlash(link); return 'Saved: the memory as it is now is kept in this browser.'; });
  $('sav-get').onclick = () => job(async () => {
    saveFile(`lsdj-sim-${stamp()}.sav`, await downloadSav(link, progress));
    return 'Downloaded the whole save.';
  });
  $('sav-file').onchange = () => {
    const f = $('sav-file').files[0];
    $('sav-file').value = '';
    if (!f || !confirm(`Replace everything in LSDj (all songs, and the one being edited) with ${f.name}? LSDj starts again.`)) return;
    job(async () => kept(`Restored ${f.name}.`, await uploadSav(link, new Uint8Array(await f.arrayBuffer()), progress)));
  };
  $('song-files').onchange = () => {
    const files = [...$('song-files').files];
    $('song-files').value = '';
    if (!files.length) return;
    job(async () => {
      const done = [];
      let last = null;
      for (const f of files) {
        try {
          last = await uploadSong(link, new Uint8Array(await f.arrayBuffer()), progress);
          done.push(f.name);
        } catch (e) {
          throw new Error(`${f.name}: ${e.message}` + (done.length ? ` (loaded before it: ${done.join(', ')})` : ''));
        }
      }
      return kept(`Loaded ${done.join(', ')}. Open it in LSDj: PROJECT, LOAD/SAVE SONG, LOAD.`, last);
    });
  };
  return { refresh: () => job(async () => '') };
}
