// Groove — app controller: library, playlists, playback, Now Playing, crossfade, Mix.
import { readMetadata, qualityInfo } from './metadata.js';
import * as db from './storage.js';
import { analyzeBlob, decodeFull } from './analyzer.js';
import { DeckWaveform, OverviewWaveform } from './waveform.js';
import { Engine } from './engine.js';
import { PRESETS, STAGES, EQ_FREQS, EQ_LABELS } from './sound.js';
import { isApp, nativeUpdate, nativeTheme, onRemote, onBack, blobToBase64, pickFiles, pickFolder, rescanFolders, releaseUri, mediaUrl, RemoteFile } from './native.js';

const $ = (id) => document.getElementById(id);
const SVGNS = 'http://www.w3.org/2000/svg';

// ---------- DOM helpers (text is always inserted as text, never parsed as HTML) ----------
function h(tag, props = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (v == null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'text') el.textContent = v;
    else if (k === 'dataset') Object.assign(el.dataset, v);
    else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
    else el.setAttribute(k, v === true ? '' : String(v));
  }
  for (const c of children.flat()) {
    if (c == null || c === false) continue;
    el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return el;
}
function icon(name) {
  const s = document.createElementNS(SVGNS, 'svg');
  const u = document.createElementNS(SVGNS, 'use');
  u.setAttribute('href', `#i-${name}`);
  s.append(u);
  return s;
}
function setIcon(btn, name) { const u = btn.querySelector('use'); if (u) u.setAttribute('href', `#i-${name}`); }
const dot = () => h('span', { class: 'meta-dot', 'aria-hidden': 'true' });
const uid = () => (crypto.randomUUID ? crypto.randomUUID() : `t${Date.now().toString(36)}${Math.random().toString(36).slice(2)}`);
const stripExt = (n) => n.replace(/\.[^/.]+$/, '');
const isDesktop = () => matchMedia('(min-width: 1181px)').matches;

// ---------- formatting ----------
function fmtTime(s) {
  if (!isFinite(s) || s < 0) s = 0;
  s = Math.floor(s);
  const hh = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return hh ? `${hh}:${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}` : `${m}:${String(sec).padStart(2, '0')}`;
}
function fmtLong(s) {
  const mins = Math.round(s / 60);
  if (mins < 60) return `${mins}min`;
  return `${Math.floor(mins / 60)}hr ${mins % 60}min`;
}
function fmtBytes(b) { return !b ? '0 MB' : b < 1e9 ? `${(b / 1e6).toFixed(b < 1e7 ? 1 : 0)} MB` : `${(b / 1e9).toFixed(2)} GB`; }
const plural = (n, w) => `${n} ${w}${n === 1 ? '' : 's'}`;
const titleOf = (t) => t.title || stripExt(t.fileName || 'Untitled');
const artistOf = (t) => t.artist || 'Unknown Artist';
const groupArtistOf = (t) => t.albumArtist || t.artist || 'Unknown Artist';
const bpmOf = (t) => (t && (t.bpmUser != null ? t.bpmUser : t.analysis && t.analysis.bpm)) || null;
const camOf = (t) => (t && t.analysis && t.analysis.camelot) || null;
const fmtBpm = (b) => (b == null ? '' : Math.abs(b - Math.round(b)) < 0.05 ? String(Math.round(b)) : b.toFixed(1));
const camelotHue = (c) => ((parseInt(c, 10) - 1) * 30 + 200) % 360;
const totalDur = (ids) => ids.reduce((a, id) => a + ((state.tracks.get(id) || {}).duration || 0), 0);

// ---------- state ----------
const state = {
  tracks: new Map(),
  playlists: new Map(),
  tab: 'songs',
  stack: [],           // detail pages: {type:'artist'|'album'|'playlist', key}
  search: '',
  sort: 'title',
  listIds: [],
  queue: [], original: null, index: -1,
  shuffle: false, repeat: 'off',
  currentId: null,
  context: 'Your library',
  artUrl: null,
  npOpen: false,
};
const settings = {
  crossfade: 0, beatmatch: true, cueSink: '', mixLen: 8, mixSync: true, fitFilter: 'best', volume: 1,
  eqOn: false, eqPreset: 'flat', eqGains: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0], stage: 'off', stageIntensity: 0.6,
};
const thumbUrls = new Map();
const analysisQueue = [];
let analyzing = null;
let autoXfadeFor = null;

const engine = new Engine({
  getBlob: (id) => sourceFor(id),
  getGrid: (id) => {
    const t = id && state.tracks.get(id);
    const a = t && t.analysis;
    const bpm = bpmOf(t);
    return a && bpm && a.beatOffset != null ? { bpm, beatOffset: a.beatOffset, downbeat: a.downbeat || 0 } : null;
  },
});

if (new URLSearchParams(location.search).has('debug')) window.grooveEngine = engine; // for testing

/** What a deck plays: the File itself (browser) or a stream URL for the file on the phone (app). */
async function sourceFor(id) {
  const t = state.tracks.get(id);
  if (t && t.uri) return t.convert ? converted(t) : mediaUrl(t.uri);
  return db.get('files', id);
}

// ---------- safety net for files Android's web player won't open or stalls on ----------
// The song is decoded in memory (the same decoder that already reads key/BPM) and handed to the
// player as plain PCM WAV: 16-bit at the file's own sample rate, kept only in memory, never saved.
const APP_VERSION = '2.2';
const convertedCache = new Map(); // id -> Promise<Blob>, at most 3 songs
function converted(t) {
  if (convertedCache.has(t.id)) {
    const p = convertedCache.get(t.id);
    convertedCache.delete(t.id); convertedCache.set(t.id, p); // most recently used last
    return p;
  }
  const p = convertToWav(t);
  convertedCache.set(t.id, p);
  p.catch(() => convertedCache.delete(t.id));
  while (convertedCache.size > 3) convertedCache.delete(convertedCache.keys().next().value);
  return p;
}
async function convertToWav(t) {
  if (t.duration && t.duration > 20 * 60) throw new Error('too long to convert on the phone');
  const ab = await new RemoteFile(t.uri, t.fileName, t.size, t.mime).arrayBuffer();
  const rate = Math.min(96000, Math.max(22050, t.sampleRate || 48000));
  const buf = await decodeFull(ab, rate);
  const n = buf.length, chs = buf.numberOfChannels;
  const ch = (i) => buf.getChannelData(Math.min(i, chs - 1));
  let L = ch(0), R = ch(1);
  if (chs > 2) { // fold surround down to stereo (ITU coefficients)
    const c = chs > 2 ? ch(2) : null, sl = chs > 4 ? ch(4) : null, sr = chs > 5 ? ch(5) : null;
    const l2 = new Float32Array(n), r2 = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      const cc = c ? c[i] * 0.7071 : 0;
      l2[i] = (L[i] + cc + (sl ? sl[i] * 0.7071 : 0)) * 0.5858;
      r2[i] = (R[i] + cc + (sr ? sr[i] * 0.7071 : 0)) * 0.5858;
    }
    L = l2; R = r2;
  }
  const out = new DataView(new ArrayBuffer(44 + n * 4));
  const str = (o, s) => { for (let i = 0; i < s.length; i++) out.setUint8(o + i, s.charCodeAt(i)); };
  str(0, 'RIFF'); out.setUint32(4, 36 + n * 4, true); str(8, 'WAVE');
  str(12, 'fmt '); out.setUint32(16, 16, true); out.setUint16(20, 1, true); out.setUint16(22, 2, true);
  out.setUint32(24, buf.sampleRate, true); out.setUint32(28, buf.sampleRate * 4, true); out.setUint16(32, 4, true); out.setUint16(34, 16, true);
  str(36, 'data'); out.setUint32(40, n * 4, true);
  for (let i = 0, o = 44; i < n; i++, o += 4) {
    const d = Math.random() - Math.random(); // TPDF dither, ±1 LSB
    out.setInt16(o, Math.max(-32768, Math.min(32767, Math.round(L[i] * 32767 + d))), true);
    out.setInt16(o + 2, Math.max(-32768, Math.min(32767, Math.round(R[i] * 32767 + d))), true);
  }
  return new Blob([out.buffer], { type: 'audio/wav' });
}
let rescuing = null;
/** Switch a song to the safety net and carry on from where it stopped. */
async function rescuePlayback(t, reason) {
  if (rescuing === t.id || t.convert) return false;
  rescuing = t.id;
  console.warn('Switching to converted playback for', t.fileName, reason);
  t.convert = true;
  db.put('tracks', t).catch(() => {});
  toast(`Preparing “${titleOf(t)}” — Android’s player couldn’t open it directly…`, { sticky: true });
  const wasCurrent = state.currentId === t.id;
  const at = wasCurrent ? engine.active.time : 0;
  try {
    await converted(t);
    if (wasCurrent && state.currentId === t.id) await engine.load(t.id, { autoplay: true, startAt: at });
    toast(`Playing “${titleOf(t)}” (converted in memory: ${reason}).`, { ms: 5000 });
  } catch (e) {
    t.convert = false;
    db.put('tracks', t).catch(() => {});
    toast(`Couldn’t play “${titleOf(t)}” (${reason}; conversion failed: ${(e && e.message) || e}).`, { ms: 8000 });
  } finally {
    rescuing = null;
    updatePlayState();
  }
  return true;
}
// Watchdog: playing, but the position hasn't moved for 5 s (the player is stuck) → safety net.
// Also covers a song that never finishes loading after you pressed play.
const stall = { id: null, t: -1, since: 0 };
let playIntent = { id: null, at: 0 };
function checkStall() {
  if (!isApp) return;
  const t = currentTrack();
  const el = engine.active.el;
  if (t && t.uri && !t.convert && playIntent.id === t.id && el.paused && el.readyState < 1 && performance.now() - playIntent.at > 8000) {
    playIntent.id = null;
    rescuePlayback(t, 'the song didn’t load');
    return;
  }
  if (!t || !t.uri || t.convert || el.paused || el.ended || el.seeking || engine.mixing) { stall.id = null; return; }
  const now = performance.now();
  if (stall.id !== t.id || Math.abs(el.currentTime - stall.t) > 0.05) { stall.id = t.id; stall.t = el.currentTime; stall.since = now; return; }
  if (now - stall.since > 5000) { stall.id = null; rescuePlayback(t, 'playback stalled'); }
}

function analysisState(t) {
  if (!t.analysis) return analysisQueue.includes(t.id) || analyzing === t.id ? 'pending' : 'none';
  return t.analysis.error ? 'error' : 'done';
}
function currentTrack() { return state.currentId ? state.tracks.get(state.currentId) : null; }
function currentDuration() {
  const d = engine.active.duration;
  if (d) return d;
  const t = currentTrack();
  return (t && (t.duration || (t.analysis && t.analysis.duration))) || 0;
}
const clk = { el: null, ct: 0, perf: 0 };
function clockTime() {
  const el = engine.active.el;
  const ct = el.currentTime || 0;
  const now = performance.now();
  if (el !== clk.el || ct !== clk.ct) { clk.el = el; clk.ct = ct; clk.perf = now; return ct; }
  if (el.paused || el.seeking) return ct;
  return Math.min(ct + ((now - clk.perf) / 1000) * el.playbackRate, ct + 0.25);
}

// ---------- Now Playing waveforms ----------
const npDeck = new DeckWaveform($('deckCanvas'), { onScrub: (t, final) => { if (final) seekTo(t); } });
const npOverview = new OverviewWaveform($('overviewCanvas'), {
  onSeek: (t) => seekTo(t), style: 'mono',
  colors: { played: '#6b3fe4', unplayed: 'rgba(107,63,228,0.28)', playhead: '#121212', empty: 'rgba(18,18,18,0.15)', emptyFill: '#6b3fe4' },
});

// =====================================================================
// Library grouping
// =====================================================================
function artistGroups() {
  const map = new Map();
  for (const t of state.tracks.values()) {
    const name = groupArtistOf(t);
    if (!map.has(name)) map.set(name, { name, ids: [] });
    map.get(name).ids.push(t.id);
  }
  return [...map.values()].sort((a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: 'base' }));
}
function albumKey(t) { return `${groupArtistOf(t)}\u0000${t.album || ''}`; }
function trackOrder(a, b) {
  return ((a.discNo || 1) - (b.discNo || 1)) || ((a.trackNo || 999) - (b.trackNo || 999)) || titleOf(a).localeCompare(titleOf(b));
}
function albumGroups(filterArtist = null) {
  const map = new Map();
  for (const t of state.tracks.values()) {
    if (!t.album) continue;
    if (filterArtist && groupArtistOf(t) !== filterArtist) continue;
    const k = albumKey(t);
    if (!map.has(k)) map.set(k, { key: k, title: t.album, artist: groupArtistOf(t), year: t.year, ids: [] });
    map.get(k).ids.push(t.id);
  }
  for (const g of map.values()) g.ids.sort((a, b) => trackOrder(state.tracks.get(a), state.tracks.get(b)));
  return [...map.values()].sort((a, b) => a.title.localeCompare(b.title, undefined, { sensitivity: 'base' }));
}
function artFor(ids) { return ids.find((id) => thumbUrls.has(id)) || null; }
function sortedPlaylists() {
  return [...state.playlists.values()].sort((a, b) => (b.system ? 1 : 0) - (a.system ? 1 : 0) || a.createdAt - b.createdAt);
}

// =====================================================================
// Rendering
// =====================================================================
const SORTS = {
  title: { label: 'Title', fn: (t) => titleOf(t).toLocaleLowerCase() },
  artist: { label: 'Artist', fn: (t) => artistOf(t).toLocaleLowerCase() },
  album: { label: 'Album', fn: (t) => (t.album || '￿').toLocaleLowerCase() },
  bpm: { label: 'BPM', fn: (t) => bpmOf(t) || 9999 },
  key: { label: 'Key', fn: (t) => { const c = camOf(t); return c ? parseInt(c, 10) * 2 + (c.endsWith('B') ? 1 : 0) : 999; } },
  recent: { label: 'Recently added', fn: (t) => -(t.addedAt || 0) },
  duration: { label: 'Duration', fn: (t) => t.duration || 0 },
};
function currentPage() { return state.stack[state.stack.length - 1] || null; }
function matchesSearch(t) {
  const q = state.search.trim().toLocaleLowerCase();
  if (!q) return true;
  const hay = [titleOf(t), t.artist, t.albumArtist, t.album, t.genre, camOf(t), t.analysis && t.analysis.keyShort].filter(Boolean).join(' ').toLocaleLowerCase();
  return q.split(/\s+/).every((w) => hay.includes(w));
}
const textMatch = (s) => { const q = state.search.trim().toLocaleLowerCase(); return !q || s.toLocaleLowerCase().includes(q); };

function render() {
  const page = currentPage();
  const theme = page ? 'theme-mustard' : state.tab === 'mix' ? 'theme-ink' : 'theme-green';
  $('mainPanel').className = `panel main-panel ${theme}${$('searchField').hidden ? '' : ' search-open'}`;
  pushTheme();
  document.querySelectorAll('.tab').forEach((t) => {
    const on = !page && t.dataset.tab === state.tab;
    t.classList.toggle('active', on);
    if (on) t.setAttribute('aria-current', 'page'); else t.removeAttribute('aria-current');
  });
  const activeTab = document.querySelector('.tab.active');
  if (activeTab) activeTab.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  $('tabs').hidden = !!page;
  $('btnBack').hidden = !page;
  $('countSongs').textContent = state.tracks.size;
  $('countArtists').textContent = artistGroups().length;
  $('countAlbums').textContent = albumGroups().length;
  $('countPlaylists').textContent = state.playlists.size;

  if (state.tab !== 'mix' || page) mixUI = null;
  const body = $('viewBody');
  $('emptyState').hidden = true;
  const libEmpty = state.tracks.size === 0;
  let title = 'Groove to the Beats', meta = '';
  let actions = { play: true, shuffle: true, sort: false, more: true };

  if (page && page.type === 'artist') {
    const ids = [...state.tracks.values()].filter((t) => groupArtistOf(t) === page.key).sort(trackOrder).map((t) => t.id);
    if (!ids.length) { state.stack.pop(); return render(); }
    title = page.key;
    meta = [plural(ids.length, 'song'), fmtLong(totalDur(ids))];
    const albums = albumGroups(page.key);
    state.listIds = ids.filter((id) => matchesSearch(state.tracks.get(id)));
    body.replaceChildren(
      albums.length ? h('div', { class: 'section-title', text: 'Albums' }) : null,
      albums.length ? albumList(albums) : null,
      h('div', { class: 'section-title', text: 'Songs' }),
      songList(state.listIds, { kind: 'artist' }),
    );
    actions.more = false;
  } else if (page && page.type === 'album') {
    const g = albumGroups().find((a) => a.key === page.key);
    if (!g) { state.stack.pop(); return render(); }
    title = g.title;
    meta = [g.artist, g.year, plural(g.ids.length, 'song'), fmtLong(totalDur(g.ids))].filter(Boolean);
    state.listIds = g.ids.filter((id) => matchesSearch(state.tracks.get(id)));
    body.replaceChildren(songList(state.listIds, { kind: 'album' }));
    actions.more = false;
  } else if (page && page.type === 'playlist') {
    const pl = state.playlists.get(page.key);
    if (!pl) { state.stack.pop(); return render(); }
    const ids = pl.trackIds.filter((id) => state.tracks.has(id));
    title = pl.name;
    meta = [plural(ids.length, 'song'), ids.length ? fmtLong(totalDur(ids)) : null].filter(Boolean);
    state.listIds = ids.filter((id) => matchesSearch(state.tracks.get(id)));
    body.replaceChildren(songList(state.listIds, { kind: 'playlist', playlistId: pl.id, reorderable: !state.search.trim() }));
    if (!ids.length) showEmpty('This playlist is empty', pl.id === 'loved' ? 'Tap the heart in Now Playing to add songs here.' : 'Use ••• › Add to playlist on any song to add it here.', false);
    actions.more = !pl.system;
  } else {
    const all = [...state.tracks.keys()];
    meta = libEmpty ? ['Your music, your mix'] : [plural(all.length, 'song'), fmtLong(totalDur(all))];
    state.listIds = sortIds(all.filter((id) => matchesSearch(state.tracks.get(id))));
    if (state.tab === 'songs') {
      body.replaceChildren(songList(state.listIds, { kind: 'library' }));
      actions.sort = true;
      if (!libEmpty && !state.listIds.length) showEmpty('No results', `Nothing matches “${state.search.trim()}”.`, false);
    } else if (state.tab === 'artists') {
      body.replaceChildren(artistCards(artistGroups().filter((g) => textMatch(g.name))));
    } else if (state.tab === 'albums') {
      const albums = albumGroups().filter((g) => textMatch(`${g.title} ${g.artist}`));
      body.replaceChildren(albumList(albums));
      if (!libEmpty && !albums.length) showEmpty('No albums', state.search ? 'Nothing matches your search.' : 'Songs with album tags show up here.', false);
    } else if (state.tab === 'playlists') {
      body.replaceChildren(playlistCards());
      actions.play = actions.shuffle = false;
    } else if (state.tab === 'mix') {
      title = 'Mix it up';
      meta = ['Cue any song next to what’s playing, preview it, then fade across.'];
      actions = { play: false, shuffle: false, sort: false, more: false };
      if (!libEmpty) renderMix(body);
    }
    if (libEmpty && state.tab !== 'playlists') {
      body.replaceChildren();
      showEmpty('Add your music', isApp
        ? 'Pick your music folder (or single songs). They play straight from your phone’s storage — nothing is copied — and they stay in Groove until you remove them. New songs you put in the folder show up automatically.'
        : 'Drop audio files or a whole folder anywhere on this page, or choose them below. Songs play straight from your files — nothing is copied or saved — and the list clears when you close this tab.', true);
      actions = { play: false, shuffle: false, sort: false, more: false };
    }
  }

  $('viewTitle').textContent = title;
  $('viewTitle').parentElement.classList.toggle('long', title.length > 22);
  $('heroNotes').style.display = page || state.tab === 'mix' ? 'none' : '';
  $('viewMeta').replaceChildren(...[].concat(meta).flatMap((m, i) => (i ? [dot(), m] : [m])));
  $('btnPlayAll').hidden = !actions.play;
  $('btnShuffleAll').hidden = !actions.shuffle;
  $('btnSort').hidden = !actions.sort;
  $('sortLabel').textContent = SORTS[state.sort].label;
  $('btnViewMore').hidden = !actions.more;
  $('heroActions').hidden = !Object.values(actions).some(Boolean);
  $('btnPlayAll').disabled = $('btnShuffleAll').disabled = !state.listIds.length;
  markCurrentRow();
}

function showEmpty(title, text, withActions) {
  $('emptyState').hidden = false;
  $('emptyTitle').textContent = title;
  $('emptyText').textContent = text;
  $('emptyActions').hidden = !withActions;
}
function sortIds(ids) {
  const fn = SORTS[state.sort].fn;
  return ids.slice().sort((a, b) => { const va = fn(state.tracks.get(a)), vb = fn(state.tracks.get(b)); return va < vb ? -1 : va > vb ? 1 : 0; });
}
function thumb(id, cls = 'thumb') {
  const url = id && thumbUrls.get(id);
  return h('span', { class: cls }, url ? h('img', { src: url, alt: '', loading: 'lazy', decoding: 'async' }) : h('span', { class: 'ph' }, icon('note')));
}
function keyNode(t) {
  const st = analysisState(t);
  if (st === 'pending') return h('span', { class: 'analyzing', text: '…', title: 'Analyzing' });
  const c = camOf(t);
  if (!c) return h('span', { class: 'muted', text: '—', title: st === 'error' ? 'This browser can’t decode this file for analysis' : 'No clear key' });
  const el = h('span', { class: 'key-chip', text: c, title: t.analysis.key });
  el.style.setProperty('--h', camelotHue(c));
  return el;
}
function bpmNode(t) {
  const st = analysisState(t);
  if (st === 'pending') return h('span', { class: 'analyzing', text: '…' });
  const b = bpmOf(t);
  return b ? document.createTextNode(fmtBpm(b)) : h('span', { class: 'muted', text: '—', title: 'No steady beat found' });
}
function shortQuality(t) {
  if (t.lossless) {
    const k = t.sampleRate ? +(t.sampleRate / 1000).toFixed(1) : null;
    const rate = k ? (k % 1 === 0 ? k.toFixed(0) : String(k)) : null;
    return [t.bitDepth && rate ? `${t.bitDepth}/${rate}` : rate ? `${rate} kHz` : '', t.codec].filter(Boolean).join(' ');
  }
  return [t.codec, t.bitrate ? String(t.bitrate) : null].filter(Boolean).join(' ');
}

function songList(ids, ctx) {
  return h('div', { class: 'songs', role: 'list' }, ids.map((id, i) => songRow(state.tracks.get(id), i, ctx)));
}
let dragFrom = null;
function songRow(t, i, ctx) {
  const sub = [artistOf(t)];
  if (ctx.kind !== 'album' && t.album) sub.push(t.album);
  const extra = [camOf(t), bpmOf(t) ? `${fmtBpm(bpmOf(t))} BPM` : null].filter(Boolean);
  const q = qualityInfo(t);
  const row = h('div', {
    class: `song-row${t.id === state.currentId ? ' current' : ''}`, role: 'listitem', tabindex: '0',
    draggable: 'true', 'aria-label': `${titleOf(t)} by ${artistOf(t)}`, dataset: { id: t.id, pos: String(i) },
  },
  thumb(t.id),
  h('span', { class: 's-text' },
    h('span', { class: 's-title' }, h('span', { class: 'eq', 'aria-hidden': 'true' }, h('i'), h('i'), h('i')), h('span', { text: titleOf(t) })),
    h('span', { class: 's-sub' }, sub.flatMap((s, k) => (k ? [dot(), s] : [s])), extra.length ? h('span', { class: 's-sub-extra' }, ...extra.flatMap((s) => [dot(), s])) : null)),
  h('span', { class: 's-quality', title: [q.badge, q.detail].filter(Boolean).join(' — '), text: shortQuality(t) }),
  h('span', { class: 's-key' }, keyNode(t)),
  h('span', { class: 's-bpm' }, bpmNode(t)),
  h('span', { class: 's-time', text: t.duration ? fmtTime(t.duration) : '' }),
  h('span', { class: 's-more' }, h('button', { class: 'circle-btn', 'aria-label': `More options for ${titleOf(t)}`, 'aria-haspopup': 'menu' }, icon('more-h'))));
  row._ctx = ctx;
  const ctxLabel = () => (ctx.kind === 'library' ? 'Your library' : $('viewTitle').textContent);
  row.addEventListener('click', (e) => {
    const more = e.target.closest('.s-more button');
    if (more) { openMenu(more, trackMenuItems(t.id, ctx, i)); return; }
    playFromList(state.listIds, t.id, { context: ctxLabel() });
  });
  row.addEventListener('keydown', (e) => { if (e.key === 'Enter' && e.target === row) { e.preventDefault(); playFromList(state.listIds, t.id, { context: ctxLabel() }); } });
  row.addEventListener('contextmenu', (e) => { e.preventDefault(); openMenu({ x: e.clientX, y: e.clientY }, trackMenuItems(t.id, ctx, i)); });
  row.addEventListener('dragstart', (e) => {
    e.dataTransfer.setData('application/x-groove-track', t.id);
    e.dataTransfer.effectAllowed = ctx.reorderable ? 'move' : 'copy';
    row.classList.add('dragging');
    dragFrom = ctx.reorderable ? i : null;
  });
  row.addEventListener('dragend', () => {
    row.classList.remove('dragging');
    document.querySelectorAll('.drop-before, .drop-after').forEach((r) => r.classList.remove('drop-before', 'drop-after'));
    dragFrom = null;
  });
  if (ctx.reorderable) {
    row.addEventListener('dragover', (e) => {
      if (dragFrom == null) return;
      e.preventDefault();
      const r = row.getBoundingClientRect();
      const after = e.clientY > r.top + r.height / 2;
      row.classList.toggle('drop-after', after);
      row.classList.toggle('drop-before', !after);
    });
    row.addEventListener('dragleave', () => row.classList.remove('drop-before', 'drop-after'));
    row.addEventListener('drop', (e) => {
      if (dragFrom == null) return;
      e.preventDefault();
      const r = row.getBoundingClientRect();
      let to = i + (e.clientY > r.top + r.height / 2 ? 1 : 0);
      if (dragFrom < to) to -= 1;
      moveInPlaylist(ctx.playlistId, dragFrom, to);
    });
  }
  return row;
}
function refreshRow(id) {
  document.querySelectorAll(`#viewBody .song-row[data-id="${CSS.escape(id)}"]`).forEach((old) => {
    const t = state.tracks.get(id);
    if (t) old.replaceWith(songRow(t, +old.dataset.pos, old._ctx));
  });
}
function markCurrentRow() {
  document.querySelectorAll('#viewBody .song-row.current').forEach((r) => r.classList.remove('current'));
  if (state.currentId) document.querySelectorAll(`#viewBody .song-row[data-id="${CSS.escape(state.currentId)}"]`).forEach((r) => r.classList.add('current'));
}

function cardKeys(open) {
  return {
    onclick: (e) => { if (!e.target.closest('.pill')) open(); },
    onkeydown: (e) => { if (e.key === 'Enter' && e.target === e.currentTarget) open(); },
  };
}
function artistCards(groups) {
  return h('div', { class: 'cards' }, groups.map((g) => {
    const art = artFor(g.ids);
    const ordered = () => g.ids.slice().sort((a, b) => trackOrder(state.tracks.get(a), state.tracks.get(b)));
    return h('div', { class: 'card', role: 'button', tabindex: '0', 'aria-label': `${g.name}, ${plural(g.ids.length, 'song')}`, ...cardKeys(() => openPage({ type: 'artist', key: g.name })) },
      h('span', { class: `card-art ring${art ? ' has-img' : ''}` }, art ? h('img', { src: thumbUrls.get(art), alt: '' }) : null),
      h('span', { class: 'card-name', text: g.name }),
      h('span', { class: 'card-meta' }, plural(g.ids.length, 'song'), dot(), fmtLong(totalDur(g.ids))),
      h('button', { class: 'pill pill-yellow', onclick: () => playFromList(ordered(), null, { shuffle: false, context: g.name }) }, icon('play'), 'Play'));
  }));
}
function albumList(albums) {
  return h('div', { class: 'albums' }, albums.map((g) => h('div', {
    class: 'album-row', role: 'button', tabindex: '0', 'aria-label': `${g.title} by ${g.artist}`,
    onclick: (e) => { if (!e.target.closest('.circle-btn')) openPage({ type: 'album', key: g.key }); },
    onkeydown: (e) => { if (e.key === 'Enter' && e.target === e.currentTarget) openPage({ type: 'album', key: g.key }); },
  },
  thumb(artFor(g.ids)),
  h('span', {}, h('div', { class: 'a-title', text: g.title }), h('div', { class: 'a-meta' }, g.artist, dot(), plural(g.ids.length, 'song'), dot(), fmtLong(totalDur(g.ids)))),
  h('button', { class: 'circle-btn', 'aria-label': `Play ${g.title}`, onclick: () => playFromList(g.ids, null, { shuffle: false, context: g.title }) }, icon('play')))));
}
function playlistCards() {
  const cards = sortedPlaylists().filter((pl) => textMatch(pl.name)).map((pl) => {
    const ids = pl.trackIds.filter((id) => state.tracks.has(id));
    const arts = ids.filter((id) => thumbUrls.has(id)).slice(0, 3);
    return h('div', { class: 'card', role: 'button', tabindex: '0', 'aria-label': `${pl.name}, ${plural(ids.length, 'song')}`, ...cardKeys(() => openPage({ type: 'playlist', key: pl.id })) },
      arts.length ? h('span', { class: 'card-stack' }, arts.map((id) => thumb(id))) : h('span', { class: 'card-art' }),
      h('span', { class: 'card-name', text: pl.name }),
      h('span', { class: 'card-meta' }, plural(ids.length, 'song'), ids.length ? [dot(), fmtLong(totalDur(ids))] : null),
      h('button', { class: 'pill pill-yellow', disabled: !ids.length || null, onclick: () => playFromList(ids, null, { shuffle: false, context: pl.name }) }, icon('play'), 'Play'));
  });
  cards.push(h('button', { class: 'card new', onclick: () => newPlaylistFlow() }, h('span', { class: 'card-name' }, icon('plus'), 'New playlist')));
  return h('div', { class: 'cards' }, cards);
}

function openPage(p) { state.stack.push(p); render(); $('content').scrollTop = 0; }
function goBack() { state.stack.pop(); render(); }
function setTab(tab) { state.tab = tab; state.stack = []; render(); $('content').scrollTop = 0; saveSession(); }

// =====================================================================
// Playback
// =====================================================================
function soundWanted() { return (settings.eqOn && settings.eqGains.some((g) => g !== 0)) || settings.stage !== 'off'; }
function maybeEnableFading() {
  if (!engine.canFade && settings.crossfade > 0) engine.enableFading();
  if (soundWanted()) applySound();
}

async function loadTrack(id, { autoplay = true, startAt = 0 } = {}) {
  const t = state.tracks.get(id);
  if (!t) return;
  state.currentId = id;
  autoXfadeFor = null;
  updateNowPlaying();
  markCurrentRow();
  renderQueue();
  prioritizeAnalysis(id);
  playIntent = autoplay ? { id, at: performance.now() } : { id: null, at: 0 };
  try {
    await engine.load(id, { autoplay, startAt });
  } catch (e) {
    if (e && e.message === 'missing file') toast(`The file for “${titleOf(t)}” is missing from storage.`);
    else if (e && e.name !== 'AbortError' && e.name !== 'NotAllowedError') console.warn(e);
  }
  updatePlayState();
  refreshMix();
  saveSession();
}
function shuffled(arr) {
  const a = arr.slice();
  for (let i = a.length - 1; i > 0; i--) { const j = Math.floor(Math.random() * (i + 1)); [a[i], a[j]] = [a[j], a[i]]; }
  return a;
}
function playFromList(ids, startId, { shuffle = state.shuffle, context } = {}) {
  if (!ids.length) return;
  maybeEnableFading();
  const start = startId ?? (shuffle ? ids[Math.floor(Math.random() * ids.length)] : ids[0]);
  state.original = ids.slice();
  state.shuffle = shuffle;
  if (context) state.context = context;
  if (shuffle) { state.queue = [start, ...shuffled(ids.filter((x) => x !== start))]; state.index = 0; }
  else { state.queue = ids.slice(); state.index = Math.max(0, ids.indexOf(start)); }
  syncModesUI();
  loadTrack(state.queue[state.index]);
}
function togglePlay() {
  maybeEnableFading();
  if (!state.currentId) { if (state.listIds.length) playFromList(state.listIds, state.listIds[0]); return; }
  if (engine.active.el.paused) {
    if (engine.active.el.readyState < 1) playIntent = { id: state.currentId, at: performance.now() };
    engine.play().catch((e) => console.warn(e));
  } else { engine.pause(); playIntent = { id: null, at: 0 }; }
}
function peekNextId() {
  if (state.index < state.queue.length - 1) return state.queue[state.index + 1];
  return state.repeat === 'all' && state.queue.length > 1 ? state.queue[0] : null;
}
function next(auto = false) {
  if (!state.queue.length) return;
  if (state.index < state.queue.length - 1) state.index++;
  else if (state.repeat === 'all') state.index = 0;
  else { if (auto) { engine.active.el.currentTime = 0; updatePlayState(); } return; }
  loadTrack(state.queue[state.index]);
}
function prev() {
  if (!state.queue.length) return;
  if (engine.active.time > 3 || (state.index === 0 && state.repeat !== 'all')) { seekTo(0); return; }
  state.index = state.index > 0 ? state.index - 1 : state.queue.length - 1;
  loadTrack(state.queue[state.index]);
}
function seekTo(sec) { if (state.currentId) engine.seek(sec); }
function toggleShuffle() {
  const cur = state.queue[state.index];
  state.shuffle = !state.shuffle;
  if (state.queue.length) {
    if (state.shuffle) { state.original = state.queue.slice(); state.queue = [cur, ...shuffled(state.original.filter((x) => x !== cur))]; state.index = 0; }
    else if (state.original) { state.queue = state.original.slice(); state.index = Math.max(0, state.queue.indexOf(cur)); }
  }
  syncModesUI(); renderQueue(); saveSession();
}
function cycleRepeat() {
  state.repeat = state.repeat === 'off' ? 'all' : state.repeat === 'all' ? 'one' : 'off';
  syncModesUI(); saveSession();
}
function playNext(id) {
  if (!state.currentId) { playFromList([id], id); return; }
  state.queue.splice(state.index + 1, 0, id);
  if (state.original && state.shuffle) state.original.splice(state.original.indexOf(state.currentId) + 1, 0, id);
  else state.original = state.queue.slice();
  renderQueue(); saveSession();
  toast(`“${titleOf(state.tracks.get(id))}” plays next.`);
}
function syncModesUI() {
  $('npShuffle').setAttribute('aria-pressed', String(state.shuffle));
  const r = $('npRepeat');
  r.setAttribute('aria-pressed', String(state.repeat !== 'off'));
  r.setAttribute('aria-label', `Repeat: ${state.repeat === 'one' ? 'one song' : state.repeat}`);
  r.title = r.getAttribute('aria-label');
  setIcon(r, state.repeat === 'one' ? 'repeat1' : 'repeat');
  engine.active.el.loop = state.repeat === 'one';
}
function updatePlayState() {
  const playing = !!state.currentId && engine.active.playing;
  for (const b of [$('npPlay'), $('miniPlay')]) { setIcon(b, playing ? 'pause' : 'play'); b.setAttribute('aria-label', playing ? 'Pause' : 'Play'); }
  document.body.classList.toggle('playing', playing);
  if (mixUI && mixUI.nowPlay) setIcon(mixUI.nowPlay, playing ? 'pause' : 'play');
  if ('mediaSession' in navigator) navigator.mediaSession.playbackState = state.currentId ? (playing ? 'playing' : 'paused') : 'none';
  pushNative();
}

// ---------- Android app shell ----------
let nativeArtKey = null;
async function pushNative() {
  if (!isApp) return;
  const t = currentTrack();
  const info = {
    hasTrack: !!t, playing: !!t && engine.active.playing,
    title: t ? titleOf(t) : '', artist: t ? artistOf(t) : '', album: (t && t.album) || '',
    duration: currentDuration(), position: engine.active.time || 0, rate: engine.active.el.playbackRate || 1,
  };
  if (t && nativeArtKey !== t.id) {
    nativeArtKey = t.id;
    const blob = await db.get('thumbs', t.id);
    info.artKey = t.id;
    info.art = blob ? await blobToBase64(blob) : '';
  }
  if (!t) nativeArtKey = null;
  nativeUpdate(info);
}
const THEME_COLORS = { 'theme-green': ['#3f8c5f', true], 'theme-mustard': ['#f2aa3c', true], 'theme-ink': ['#1b1b1b', false] };
function pushTheme() {
  if (!isApp) return;
  if (state.npOpen) { nativeTheme('#d7d4fc', true); return; }
  if (matchMedia('(min-width: 721px)').matches) { nativeTheme('#111111', false); return; }
  const cls = [...$('mainPanel').classList].find((c) => THEME_COLORS[c]);
  const [color, light] = THEME_COLORS[cls] || ['#111111', false];
  nativeTheme(color, light);
}

async function updateNowPlaying() {
  const t = currentTrack();
  const id = t && t.id;
  let artUrl = null;
  if (t && t.hasArt) {
    const art = await db.get('art', t.id).catch(() => null);
    if (state.currentId !== id) return; // a newer track was loaded meanwhile
    if (art) artUrl = URL.createObjectURL(art);
  }
  if (state.artUrl) URL.revokeObjectURL(state.artUrl);
  state.artUrl = artUrl;
  $('npTitle').textContent = t ? titleOf(t) : 'Nothing playing';
  $('npArtist').textContent = t ? [artistOf(t), t.album].filter(Boolean).join(' — ') : 'Add music and press play';
  $('npFrom').textContent = t ? state.context : 'Your library';
  for (const img of [$('npArt'), $('vinylLabel')]) {
    if (artUrl) { img.src = artUrl; img.hidden = false; } else { img.removeAttribute('src'); img.hidden = true; }
  }
  document.title = t ? `${titleOf(t)} — ${artistOf(t)}` : 'Groove';
  const q = $('npQuality');
  if (t) {
    const qi = qualityInfo(t);
    q.replaceChildren(...(qi.badge ? [h('b', { text: qi.badge })] : []), h('span', { class: 'q-detail', text: qi.detail }));
    q.hidden = !qi.badge && !qi.detail;
  } else q.hidden = true;
  const loved = !!t && ((state.playlists.get('loved') || {}).trackIds || []).includes(t.id);
  const love = $('npLove');
  love.disabled = !t;
  love.setAttribute('aria-pressed', String(loved));
  love.setAttribute('aria-label', loved ? 'Remove from Loved Songs' : 'Add to Loved Songs');
  setIcon(love, loved ? 'heart-fill' : 'heart');
  $('mini').hidden = !t;
  if (t) {
    const mt = thumb(t.id); mt.id = 'miniThumb';
    $('miniThumb').replaceWith(mt);
    $('miniTitle').textContent = titleOf(t);
    $('miniSub').textContent = artistOf(t);
  }
  updateAnalysisUI();
  if ('mediaSession' in navigator && t) {
    navigator.mediaSession.metadata = new MediaMetadata({
      title: titleOf(t), artist: artistOf(t), album: t.album || '',
      artwork: artUrl ? [{ src: artUrl, sizes: '512x512' }] : [{ src: 'icon.svg', sizes: 'any', type: 'image/svg+xml' }],
    });
  }
}
function gridData(t) {
  const a = t && t.analysis && !t.analysis.error ? t.analysis : null;
  return a ? { ...a, bpm: bpmOf(t) } : null;
}
function updateAnalysisUI() {
  const t = currentTrack();
  const st = t ? analysisState(t) : 'none';
  const a = gridData(t);
  const keyEl = $('npKey');
  if (a && a.camelot) { keyEl.textContent = `${a.key} · ${a.camelot}`; keyEl.classList.add('has-key'); keyEl.style.setProperty('--h', camelotHue(a.camelot)); }
  else { keyEl.textContent = st === 'pending' ? 'Key …' : 'Key —'; keyEl.classList.remove('has-key'); }
  const b = bpmOf(t);
  $('npBpm').textContent = b ? `${fmtBpm(b)} BPM` : st === 'pending' ? 'BPM …' : 'BPM —';
  $('npBpm').disabled = !b;
  npDeck.setData(a, currentDuration());
  npOverview.setData(a, currentDuration());
  const s = $('deckStatus');
  if (!t) { s.hidden = false; s.textContent = 'Nothing playing'; }
  else if (st === 'pending' || st === 'none') { s.hidden = false; s.textContent = 'Reading waveform, key and tempo…'; }
  else if (st === 'error') { s.hidden = false; s.textContent = 'This browser can’t decode this file for analysis.'; }
  else s.hidden = true;
}
function renderQueue() {
  const upcoming = state.queue.slice(state.index + 1, state.index + 101);
  $('queueList').replaceChildren(...upcoming.map((id, k) => {
    const t = state.tracks.get(id);
    if (!t) return null;
    return h('li', {}, h('button', { class: 'queue-item', onclick: () => { state.index = state.index + 1 + k; loadTrack(state.queue[state.index]); } },
      thumb(id),
      h('span', { class: 's-text' }, h('span', { class: 'q-title', text: titleOf(t) }), h('span', { class: 'q-sub', text: artistOf(t) })),
      h('span', { class: 'q-key', text: [camOf(t), bpmOf(t) ? fmtBpm(bpmOf(t)) : null].filter(Boolean).join(' · ') })));
  }).filter(Boolean));
  $('queueEmpty').hidden = upcoming.length > 0;
  $('btnXfadeQuick').textContent = settings.crossfade ? `Crossfade ${settings.crossfade}s` : 'Crossfade off';
  $('btnXfadeQuick').setAttribute('aria-pressed', String(!!settings.crossfade));
}

function openNowPlaying() {
  if (!state.currentId || isDesktop()) return;
  state.npOpen = true;
  $('nowPlaying').classList.add('open');
  $('nowPlaying').removeAttribute('inert');
  pushTheme();
  requestAnimationFrame(() => $('npClose').focus({ preventScroll: true }));
}
function closeNowPlaying() {
  state.npOpen = false;
  $('nowPlaying').classList.remove('open');
  if (!isDesktop()) $('nowPlaying').setAttribute('inert', '');
  pushTheme();
}
function syncNpInert() {
  if (isDesktop()) $('nowPlaying').removeAttribute('inert');
  else if (!state.npOpen) $('nowPlaying').setAttribute('inert', '');
}

// =====================================================================
// Auto crossfade + animation loop
// =====================================================================
function canBeatmatch(aId, bId) {
  const ga = engine.getGrid(aId), gb = engine.getGrid(bId);
  return !!(ga && gb && engine.bestMultiplier(ga.bpm, gb.bpm));
}
function maybeAutoCrossfade() {
  const s = settings.crossfade;
  if (!s || !engine.canFade || engine.mixing || !engine.active.playing || state.repeat === 'one') return;
  const cur = engine.active.trackId;
  if (!cur || autoXfadeFor === cur) return;
  const d = engine.active.duration, t = engine.active.time;
  if (!d || d < s * 2.5) return;
  const cued = engine.other.trackId;
  const nextId = cued || peekNextId();
  if (!nextId) return;
  const lead = settings.beatmatch && canBeatmatch(cur, nextId) ? 2.4 : 0.15;
  if (d - t > s + lead) return;
  autoXfadeFor = cur;
  engine.syncEnabled = settings.beatmatch;
  if (cued) engine.mixTo(s, { kind: 'auto' });
  else engine.autoCrossfade(nextId, s);
}

let lastSec = -1;
function frame() {
  requestAnimationFrame(frame);
  engine.tick();
  maybeAutoCrossfade();
  if (mixUI) tickMix();
  if (!state.currentId) return;
  const t = clockTime();
  const d = currentDuration();
  const sec = Math.floor(t);
  if (sec !== lastSec) {
    lastSec = sec;
    $('npElapsed').textContent = fmtTime(t);
    $('npRemaining').textContent = `-${fmtTime(Math.max(0, d - t))}`;
  }
  $('miniProgress').style.width = d ? `${(t / d) * 100}%` : '0%';
  if (isDesktop() || state.npOpen) {
    if (!npDeck.dragging) npDeck.render(t);
    npOverview.render(d ? t / d : 0);
  }
  const note = engine.pendingStart ? 'Waiting for the next bar…' : engine.mix ? (engine.mix.kind === 'auto' ? 'Crossfading…' : 'Mixing…') : '';
  if ($('xfadeNote').textContent !== note) $('xfadeNote').textContent = note;
}
function fill(range) {
  const min = +range.min || 0, max = +range.max || 1;
  range.style.setProperty('--p', `${((+range.value - min) / (max - min)) * 100}%`);
}

// =====================================================================
// Mix screen
// =====================================================================
let mixUI = null;

function camelotParse(c) { const m = /^(\d+)([AB])$/.exec(c || ''); return m ? { n: +m[1], l: m[2] } : null; }
function keyRelation(ca, cb) {
  const a = camelotParse(ca), b = camelotParse(cb);
  if (!a || !b) return null;
  const d = (((b.n - a.n) % 12) + 12) % 12;
  if (d === 0 && a.l === b.l) return { label: 'Same key', score: 3, cls: 'great' };
  if (d === 0) return { label: 'Relative key', score: 2.5, cls: 'great' };
  if (a.l === b.l && (d === 1 || d === 11)) return { label: d === 1 ? 'Energy +1' : 'Energy −1', score: 2.5, cls: 'great' };
  if (a.l === b.l && d === 2) return { label: 'Energy boost', score: 1.5, cls: 'good' };
  if (a.l !== b.l && ((a.l === 'A' && d === 1) || (a.l === 'B' && d === 11))) return { label: 'Mood shift', score: 1.5, cls: 'good' };
  if (a.l === b.l && d === 7) return { label: 'Key lift', score: 1, cls: 'ok' };
  return { label: 'Key clash', score: 0, cls: 'ok' };
}
function tempoRelation(bpmA, bpmB) {
  if (!bpmA || !bpmB) return null;
  const best = engine.bestMultiplier(bpmA, bpmB);
  if (!best) return { label: `${fmtBpm(bpmB)} BPM`, score: 0, syncable: false };
  const pct = (best.r - 1) * 100;
  const label = `${pct >= 0 ? '+' : '−'}${Math.abs(pct).toFixed(1)}%${best.m === 2 ? ' · half-time' : best.m === 0.5 ? ' · double-time' : ''}`;
  return { label, score: Math.abs(pct) <= 3 ? 2 : Math.abs(pct) <= 6 ? 1 : 0.5, syncable: true, pct };
}

function renderMix(body) {
  const ui = {};
  ui.nowCard = h('section', { class: 'deck-card now', 'aria-label': 'Now playing deck' });
  ui.nextCard = h('section', { class: 'deck-card next', 'aria-label': 'Next deck' });
  ui.fader = h('input', { type: 'range', min: '0', max: '1000', value: '0', 'aria-label': 'Crossfader: now playing to next' });
  ui.mixBtn = h('button', { class: 'pill pill-yellow' }, icon('mix'), h('span', { text: 'Mix' }));
  ui.syncBtn = h('button', { class: 'pill pill-outline small', 'aria-pressed': String(settings.mixSync), title: 'Match tempo and lock the beats together' }, icon('sync'), 'Sync');
  ui.seg = h('div', { class: 'seg', role: 'group', 'aria-label': 'Mix length' }, [4, 8, 16, 32].map((s) => h('button', { 'aria-pressed': String(settings.mixLen === s), dataset: { s: String(s) }, text: `${s}s` })));
  ui.phaseMeter = h('span', { class: 'phase-meter' }, h('i'));
  ui.phase = h('span', { class: 'phase', hidden: true }, 'Beats', ui.phaseMeter);
  const xf = h('section', { class: 'xfader', 'aria-label': 'Crossfader' },
    h('div', { class: 'xf-row' }, h('span', { class: 'xf-tag now', text: 'NOW' }), ui.fader, h('span', { class: 'xf-tag next', text: 'NEXT' })),
    h('div', { class: 'xf-controls' }, ui.mixBtn, ui.seg, ui.syncBtn, h('span', { class: 'spacer' }), ui.phase));
  ui.fitsBody = h('div', { class: 'fits' });
  ui.filterSeg = h('div', { class: 'seg', role: 'group', 'aria-label': 'Filter' }, [['best', 'Best fits'], ['all', 'All songs']].map(([v, l]) => h('button', { 'aria-pressed': String(settings.fitFilter === v), dataset: { v }, text: l })));
  const fits = h('section', {}, h('div', { class: 'fits-head' }, h('h3', { text: 'What fits next' }), ui.filterSeg), ui.fitsBody);
  body.replaceChildren(h('div', { class: 'mix' }, h('div', { class: 'decks' }, ui.nowCard, ui.nextCard, xf), fits));

  ui.fader.addEventListener('input', () => {
    engine.enableFading();
    engine.syncEnabled = settings.mixSync;
    ui.dragging = true;
    fill(ui.fader);
    engine.setCrossfader(+ui.fader.value / 1000);
  });
  ui.fader.addEventListener('change', () => {
    ui.dragging = false;
    engine.setCrossfader(+ui.fader.value / 1000, { release: true });
  });
  ui.mixBtn.addEventListener('click', () => {
    if (!engine.other.trackId) { toast('Cue a song first — pick one from the list below.'); return; }
    if (!state.currentId || !engine.active.playing) { toast('Start playing something first.'); return; }
    engine.enableFading();
    engine.syncEnabled = settings.mixSync;
    engine.mixTo(settings.mixLen, { kind: 'manual' });
  });
  ui.syncBtn.addEventListener('click', () => {
    settings.mixSync = !settings.mixSync;
    ui.syncBtn.setAttribute('aria-pressed', String(settings.mixSync));
    engine.syncEnabled = settings.mixSync;
    if (engine.other.playing) {
      if (settings.mixSync) engine.lockTempo();
      else { engine.sync = null; engine.other.el.playbackRate = 1; }
    }
    saveSettings();
  });
  ui.seg.addEventListener('click', (e) => {
    const b = e.target.closest('button'); if (!b) return;
    settings.mixLen = +b.dataset.s;
    ui.seg.querySelectorAll('button').forEach((x) => x.setAttribute('aria-pressed', String(x === b)));
    saveSettings();
  });
  ui.filterSeg.addEventListener('click', (e) => {
    const b = e.target.closest('button'); if (!b) return;
    settings.fitFilter = b.dataset.v;
    ui.filterSeg.querySelectorAll('button').forEach((x) => x.setAttribute('aria-pressed', String(x === b)));
    saveSettings(); renderFits();
  });
  mixUI = ui;
  refreshMixDecks();
  renderFits();
}

function deckHeader(label, t, extraChips = []) {
  const chips = [];
  if (t) {
    const c = camOf(t);
    if (c) { const k = h('span', { class: 'chip chip-key has-key', text: `${t.analysis.keyShort || ''} · ${c}` }); k.style.setProperty('--h', camelotHue(c)); chips.push(k); }
    const b = bpmOf(t);
    chips.push(h('span', { class: 'chip', text: b ? `${fmtBpm(b)} BPM` : 'BPM —' }));
  }
  return h('div', { class: 'deck-top' }, thumb(t && t.id),
    h('div', { class: 'deck-info' }, h('div', { class: 'deck-label', text: label }), h('div', { class: 'deck-name', text: t ? titleOf(t) : 'Nothing playing' }), h('div', { class: 'deck-artist', text: t ? artistOf(t) : '' })),
    h('div', { class: 'deck-chips' }, chips, extraChips));
}

function refreshMixDecks() {
  const ui = mixUI;
  if (!ui) return;
  const nt = currentTrack();
  ui.nowRate = h('span', { class: 'chip', hidden: true });
  ui.nowPlay = h('button', { class: 'circle-btn', 'aria-label': 'Play or pause', onclick: togglePlay }, icon(engine.active.playing ? 'pause' : 'play'));
  ui.nowTime = h('span', { class: 'deck-time' });
  const nowWave = h('canvas', { 'aria-label': 'Now playing waveform. Drag to scrub.' });
  const nowOver = h('canvas', { class: 'deck-over', 'aria-label': 'Now playing overview. Tap to seek.' });
  ui.nowCard.replaceChildren(
    deckHeader('Now playing', nt, [ui.nowRate]),
    h('div', { class: 'deck-wave' }, nowWave, nt ? null : h('div', { class: 'deck-status', text: 'Play a song to start' })),
    nowOver,
    h('div', { class: 'deck-actions' }, ui.nowPlay, h('span', { class: 'grow' }), ui.nowTime));
  ui.nowDeck = new DeckWaveform(nowWave, { onScrub: (t, final) => { if (final) seekTo(t); } });
  ui.nowOver = new OverviewWaveform(nowOver, { onSeek: (t) => seekTo(t) });
  ui.nowDeck.setData(gridData(nt), currentDuration());
  ui.nowOver.setData(gridData(nt), currentDuration());

  const xt = engine.other.trackId ? state.tracks.get(engine.other.trackId) : null;
  if (!xt) {
    ui.nextCard.replaceChildren(h('div', { class: 'deck-empty' }, h('b', { text: 'Next deck is empty' }), h('span', { text: 'Pick any song below to cue it here. Preview it against what’s playing, then fade across.' })));
    ui.nextDeck = ui.nextOver = null;
    return;
  }
  const rel = nt ? keyRelation(camOf(nt), camOf(xt)) : null;
  ui.nextRate = h('span', { class: 'chip', hidden: true });
  ui.cuePlay = h('button', { class: 'circle-btn', 'aria-label': 'Preview play or pause' }, icon(engine.other.playing ? 'pause' : 'play'));
  ui.listen = h('button', { class: 'pill pill-outline small', 'aria-pressed': String(engine.cueListen), title: 'Hear the next deck quietly while the main song plays' }, icon('headphones'), 'Listen');
  ui.nextTime = h('span', { class: 'deck-time' });
  const unload = h('button', { class: 'circle-btn', 'aria-label': 'Remove from next deck' }, icon('x'));
  const nextWave = h('canvas', { 'aria-label': 'Next deck waveform. Drag to move the cue point.' });
  const nextOver = h('canvas', { class: 'deck-over', 'aria-label': 'Next deck overview. Tap to move the cue point.' });
  ui.nextCard.replaceChildren(
    deckHeader('Next · cued', xt, [rel ? h('span', { class: `match ${rel.cls}`, text: rel.label }) : null, ui.nextRate]),
    h('div', { class: 'deck-wave' }, nextWave, xt.analysis && !xt.analysis.error ? null : h('div', { class: 'deck-status', text: xt.analysis ? 'Can’t analyse this file' : 'Analyzing…' })),
    nextOver,
    h('div', { class: 'deck-actions' }, ui.cuePlay, ui.listen, unload, h('span', { class: 'grow' }), ui.nextTime));
  ui.nextDeck = new DeckWaveform(nextWave, { onScrub: (t) => engine.cueSeek(t) });
  ui.nextOver = new OverviewWaveform(nextOver, { onSeek: (t) => engine.cueSeek(t) });
  const nd = engine.other.duration || xt.duration;
  ui.nextDeck.setData(gridData(xt), nd);
  ui.nextOver.setData(gridData(xt), nd);
  ui.cuePlay.addEventListener('click', async () => {
    engine.enableFading();
    engine.syncEnabled = settings.mixSync;
    // First preview: make it audible (quietly) unless a separate headphone output is chosen.
    if (!engine.other.playing && !engine.cueListen && engine.x === 0 && !engine.cueSink) {
      engine.cueListen = true;
      ui.listen.setAttribute('aria-pressed', 'true');
      engine.applyLevels();
    }
    await engine.cueToggle();
    setIcon(ui.cuePlay, engine.other.playing ? 'pause' : 'play');
  });
  ui.listen.addEventListener('click', () => {
    engine.enableFading();
    engine.cueListen = !engine.cueListen;
    ui.listen.setAttribute('aria-pressed', String(engine.cueListen));
    engine.applyLevels();
  });
  unload.addEventListener('click', () => { engine.cancelMix(); refreshMixDecks(); renderFits(); });
}

function renderFits() {
  const ui = mixUI;
  if (!ui) return;
  const cur = currentTrack();
  const cueId = engine.other.trackId;
  let rows = [...state.tracks.values()].filter((t) => t.id !== state.currentId && matchesSearch(t)).map((t) => {
    const kr = cur ? keyRelation(camOf(cur), camOf(t)) : null;
    const tr = cur ? tempoRelation(bpmOf(cur), bpmOf(t)) : null;
    return { t, kr, tr, score: (kr ? kr.score : 0) + (tr ? tr.score : 0) };
  });
  if (cur && settings.fitFilter === 'best') rows = rows.filter((r) => r.kr && r.kr.score >= 1.5 && r.tr && r.tr.syncable);
  rows.sort((a, b) => b.score - a.score || titleOf(a.t).localeCompare(titleOf(b.t)));
  rows = rows.slice(0, 200);
  if (!rows.length) {
    ui.fitsBody.replaceChildren(h('p', { class: 'muted', text: cur ? 'No close matches yet. Try “All songs”, or wait for the analysis to finish.' : 'Play a song first to see which songs fit with it.' }));
    return;
  }
  ui.fitsBody.replaceChildren(...rows.map(({ t, kr, tr }) => h('div', { class: `fit-row${t.id === cueId ? ' loaded' : ''}` },
    thumb(t.id),
    h('span', { class: 's-text' }, h('span', { class: 's-title' }, h('span', { text: titleOf(t) })), h('span', { class: 's-sub' }, artistOf(t), camOf(t) ? [dot(), camOf(t)] : null, bpmOf(t) ? [dot(), `${fmtBpm(bpmOf(t))} BPM`] : null)),
    h('span', { class: 'fit-badges' }, kr ? h('span', { class: `match ${kr.cls}`, text: kr.label }) : null, tr ? h('span', { class: 'match tempo', text: tr.label }) : null),
    h('button', { class: 'pill pill-outline small', 'aria-pressed': String(t.id === cueId), onclick: () => cueTrack(t.id) }, t.id === cueId ? 'Cued' : 'Cue'))));
}

async function cueTrack(id) {
  engine.enableFading();
  try {
    await engine.cue(id);
    refreshMix();
    prioritizeAnalysis(id);
  } catch (e) {
    toast('This song couldn’t be loaded.');
    console.warn(e);
  }
}

function tickMix() {
  const ui = mixUI;
  const t = clockTime();
  const d = currentDuration();
  if (ui.nowDeck && !ui.nowDeck.dragging) ui.nowDeck.render(t);
  if (ui.nowOver) ui.nowOver.render(d ? t / d : 0);
  if (ui.nowTime) ui.nowTime.textContent = state.currentId ? `${fmtTime(t)} / ${fmtTime(d)}` : '';
  const ra = engine.active.el.playbackRate;
  if (ui.nowRate) {
    ui.nowRate.hidden = Math.abs(ra - 1) < 0.001;
    if (!ui.nowRate.hidden) ui.nowRate.textContent = `${ra > 1 ? '+' : '−'}${Math.abs((ra - 1) * 100).toFixed(1)}%`;
  }
  if (ui.nextDeck) {
    const o = engine.other;
    const od = o.duration;
    if (!ui.nextDeck.dragging) ui.nextDeck.render(o.time);
    ui.nextOver.duration = od;
    ui.nextOver.render(od ? o.time / od : 0);
    ui.nextTime.textContent = `${fmtTime(o.time)} / ${fmtTime(od)}`;
    const rb = o.el.playbackRate;
    ui.nextRate.hidden = Math.abs(rb - 1) < 0.001;
    if (!ui.nextRate.hidden) ui.nextRate.textContent = `Synced ${rb > 1 ? '+' : '−'}${Math.abs((rb - 1) * 100).toFixed(1)}%`;
    const want = `#i-${o.playing ? 'pause' : 'play'}`;
    if (ui.cuePlay.querySelector('use').getAttribute('href') !== want) setIcon(ui.cuePlay, o.playing ? 'pause' : 'play');
  }
  if (!ui.dragging) {
    const v = Math.round(engine.x * 1000);
    if (+ui.fader.value !== v) { ui.fader.value = v; fill(ui.fader); }
  }
  const showPhase = !!engine.sync && engine.other.playing && engine.lastPhaseError != null;
  ui.phase.hidden = !showPhase;
  if (showPhase) ui.phaseMeter.firstChild.style.left = `${50 + engine.lastPhaseError * 90}%`;
  ui.mixBtn.disabled = !engine.other.trackId || !!engine.mix || !!engine.pendingStart;
  const label = engine.pendingStart ? 'On the next bar…' : engine.mix ? 'Mixing…' : `Mix in ${settings.mixLen}s`;
  if (ui.mixBtn.lastChild.textContent !== label) ui.mixBtn.lastChild.textContent = label;
}
function refreshMix() { if (mixUI) { refreshMixDecks(); renderFits(); } }

// =====================================================================
// Menus & dialogs
// =====================================================================
let menuCleanup = null;
function openMenu(anchor, items) {
  closeMenu();
  const m = $('menu');
  m.replaceChildren(...items.filter(Boolean).map((it) => {
    if (it === 'sep') return h('div', { class: 'menu-sep', role: 'separator' });
    const b = h('button', { class: `menu-item${it.danger ? ' danger' : ''}`, role: it.checked != null ? 'menuitemradio' : 'menuitem', 'aria-checked': it.checked != null ? String(!!it.checked) : null, disabled: it.disabled || null }, it.icon ? icon(it.icon) : null, h('span', { text: it.label }));
    b.addEventListener('click', () => { closeMenu(); it.action(); });
    return b;
  }));
  m.hidden = false;
  const mw = m.offsetWidth, mh = m.offsetHeight;
  let x, y;
  if (anchor instanceof Element) {
    const r = anchor.getBoundingClientRect();
    x = r.right - mw; y = r.bottom + 6;
    if (y + mh > innerHeight - 8) y = r.top - mh - 6;
    anchor.setAttribute('aria-expanded', 'true');
  } else { x = anchor.x; y = anchor.y; }
  m.style.left = `${Math.max(8, Math.min(x, innerWidth - mw - 8))}px`;
  m.style.top = `${Math.max(8, Math.min(y, innerHeight - mh - 8))}px`;
  const first = m.querySelector('.menu-item:not([disabled])');
  if (first) first.focus({ preventScroll: true });
  const onDown = (e) => { if (!m.contains(e.target)) closeMenu(); };
  const onKey = (e) => {
    const list = [...m.querySelectorAll('.menu-item:not([disabled])')];
    const i = list.indexOf(document.activeElement);
    if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); closeMenu(); if (anchor instanceof Element) anchor.focus(); }
    else if (e.key === 'ArrowDown') { e.preventDefault(); list[(i + 1) % list.length]?.focus(); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); list[(i - 1 + list.length) % list.length]?.focus(); }
    else if (e.key === 'Tab') closeMenu();
  };
  setTimeout(() => document.addEventListener('pointerdown', onDown, true), 0);
  document.addEventListener('keydown', onKey, true);
  window.addEventListener('resize', closeMenu, { once: true });
  menuCleanup = () => {
    document.removeEventListener('pointerdown', onDown, true);
    document.removeEventListener('keydown', onKey, true);
    if (anchor instanceof Element) anchor.removeAttribute('aria-expanded');
  };
}
function closeMenu() {
  const m = $('menu');
  if (m.hidden) return;
  m.hidden = true; m.replaceChildren();
  if (menuCleanup) { menuCleanup(); menuCleanup = null; }
}
function trackMenuItems(id, ctx = {}, pos = 0) {
  const t = state.tracks.get(id);
  const inPl = ctx.kind === 'playlist';
  const plLen = inPl ? state.playlists.get(ctx.playlistId).trackIds.length : 0;
  return [
    { label: 'Play next', icon: 'next-up', action: () => playNext(id) },
    { label: 'Cue in Mix', icon: 'mix', action: () => { setTab('mix'); cueTrack(id); } },
    { label: 'Add to playlist…', icon: 'playlist', action: () => pickPlaylistFor([id]) },
    ctx.kind !== 'artist' ? { label: `Go to ${groupArtistOf(t)}`, icon: 'note', action: () => { state.stack = []; openPage({ type: 'artist', key: groupArtistOf(t) }); } } : null,
    { label: 'Get info', icon: 'info', action: () => showInfo(id) },
    inPl && ctx.reorderable ? 'sep' : null,
    inPl && ctx.reorderable ? { label: 'Move up', action: () => moveInPlaylist(ctx.playlistId, pos, pos - 1), disabled: pos === 0 } : null,
    inPl && ctx.reorderable ? { label: 'Move down', action: () => moveInPlaylist(ctx.playlistId, pos, pos + 1), disabled: pos >= plLen - 1 } : null,
    'sep',
    inPl ? { label: 'Remove from playlist', icon: 'trash', action: () => removeFromPlaylist(ctx.playlistId, id) } : null,
    { label: 'Delete from library…', icon: 'trash', danger: true, action: () => deleteTrackFlow(id, titleOf(t)) },
  ];
}
function playlistMenuItems(plId) {
  return [
    { label: 'Rename…', action: () => renamePlaylistFlow(plId) },
    { label: 'Delete playlist…', icon: 'trash', danger: true, action: () => deletePlaylistFlow(plId) },
  ];
}
function promptDialog(title, value = '', okLabel = 'Create') {
  const dlg = $('dlgPrompt'), input = $('dlgPromptInput');
  $('dlgPromptTitle').textContent = title;
  $('dlgPromptOk').textContent = okLabel;
  input.value = value;
  dlg.returnValue = '';
  dlg.showModal();
  input.focus(); input.select();
  return new Promise((resolve) => dlg.addEventListener('close', () => resolve(dlg.returnValue === 'ok' ? input.value.trim() || null : null), { once: true }));
}
function confirmDialog(title, text, okLabel = 'OK') {
  const dlg = $('dlgConfirm');
  $('dlgConfirmTitle').textContent = title;
  $('dlgConfirmText').textContent = text;
  $('dlgConfirmOk').textContent = okLabel;
  dlg.returnValue = '';
  dlg.showModal();
  return new Promise((resolve) => dlg.addEventListener('close', () => resolve(dlg.returnValue === 'ok'), { once: true }));
}
function pickPlaylistFor(ids) {
  const dlg = $('dlgPick');
  $('dlgPickList').replaceChildren(
    h('button', { type: 'button', class: 'pick-item', onclick: async () => { dlg.close(); const id = await newPlaylistFlow(false); if (id) addToPlaylist(id, ids); } }, icon('plus'), h('span', { text: 'New playlist…' })),
    ...sortedPlaylists().map((pl) => h('button', { type: 'button', class: 'pick-item', onclick: () => { dlg.close(); addToPlaylist(pl.id, ids); } },
      icon(pl.id === 'loved' ? 'heart' : 'playlist'), h('span', { text: pl.name }), h('span', { class: 'pick-count', text: String(pl.trackIds.length) }))));
  dlg.showModal();
}
function showInfo(id) {
  const t = state.tracks.get(id);
  if (!t) return;
  $('infoTitle').textContent = titleOf(t);
  $('infoSub').textContent = [artistOf(t), t.album].filter(Boolean).join(' — ');
  const th = thumb(id, 'thumb big'); th.id = 'infoThumb';
  $('infoThumb').replaceWith(th);
  const q = qualityInfo(t);
  const a = t.analysis && !t.analysis.error ? t.analysis : null;
  const rows = [
    ['Format', [t.codec, t.lossless === true ? 'lossless' : t.lossless === false ? 'lossy' : null].filter(Boolean).join(', ')],
    ['Quality', q.badge],
    ['Sample rate', t.sampleRate ? `${(t.sampleRate / 1000).toFixed(t.sampleRate % 1000 ? 1 : 0)} kHz` : null],
    ['Bit depth', t.bitDepth ? `${t.bitDepth}-bit` : null],
    ['Channels', t.channels ? (t.channels === 1 ? 'Mono' : t.channels === 2 ? 'Stereo' : `${t.channels}`) : null],
    ['Bit rate', t.bitrate ? `${t.bitrate} kbps` : null],
    ['Duration', t.duration ? fmtTime(t.duration) : null],
    ['Key', a && a.key ? `${a.key} (${a.camelot})` : a ? 'No clear key' : null],
    ['BPM', bpmOf(t) ? fmtBpm(bpmOf(t)) + (t.bpmUser != null ? ' (adjusted)' : '') : a ? 'No steady beat' : null],
    ['Genre', t.genre], ['Year', t.year], ['Track', t.trackNo ? String(t.trackNo) : null],
    ['File', t.fileName], ['Size', t.size ? fmtBytes(t.size) : null],
    ['Added', t.addedAt ? new Date(t.addedAt).toLocaleDateString() : null],
  ].filter(([, v]) => v);
  $('infoList').replaceChildren(...rows.flatMap(([k, v]) => [h('dt', { text: k }), h('dd', { text: v })]));
  $('dlgInfo').showModal();
}
let toastTimer = null;
function toast(msg, { sticky = false, ms = 3200 } = {}) {
  const el = $('toast');
  el.textContent = msg;
  el.hidden = false;
  clearTimeout(toastTimer);
  if (!sticky) toastTimer = setTimeout(() => { el.hidden = true; }, ms);
}

// ---------- opening animation ----------
const SPLASH_MIN_MS = 1500;
function hideSplash() {
  const el = $('splash');
  if (!el || el.classList.contains('done')) return;
  const finish = () => {
    if (el.classList.contains('done')) return;
    el.classList.add('done');
    setTimeout(() => el.remove(), 700);
  };
  el.addEventListener('click', finish, { once: true });
  // performance.now() counts from page load, so a slow start doesn't add extra waiting.
  setTimeout(finish, Math.max(0, SPLASH_MIN_MS - performance.now()));
}

// =====================================================================
// Library & playlist operations
// =====================================================================
async function newPlaylistFlow(navigate = true) {
  const name = await promptDialog('New playlist', '', 'Create');
  if (!name) return null;
  const pl = { id: uid(), name, trackIds: [], createdAt: Date.now() };
  state.playlists.set(pl.id, pl);
  await db.put('playlists', pl);
  if (navigate) { state.tab = 'playlists'; state.stack = []; openPage({ type: 'playlist', key: pl.id }); } else render();
  return pl.id;
}
async function renamePlaylistFlow(id) {
  const pl = state.playlists.get(id);
  if (!pl || pl.system) return;
  const name = await promptDialog('Rename playlist', pl.name, 'Rename');
  if (!name || name === pl.name) return;
  pl.name = name;
  await db.put('playlists', pl);
  render();
}
async function deletePlaylistFlow(id) {
  const pl = state.playlists.get(id);
  if (!pl || pl.system) return;
  if (!(await confirmDialog(`Delete “${pl.name}”?`, 'The songs stay in your library.', 'Delete'))) return;
  state.playlists.delete(id);
  await db.deletePlaylist(id);
  state.stack = state.stack.filter((p) => !(p.type === 'playlist' && p.key === id));
  render();
}
async function addToPlaylist(plId, ids, { silent = false } = {}) {
  const pl = state.playlists.get(plId);
  if (!pl) return;
  const fresh = ids.filter((id) => !pl.trackIds.includes(id));
  if (!fresh.length) { if (!silent) toast(`Already in “${pl.name}”.`); return; }
  pl.trackIds.push(...fresh);
  await db.put('playlists', pl);
  render();
  if (!silent) toast(fresh.length === 1 ? `Added to “${pl.name}”.` : `Added ${fresh.length} songs to “${pl.name}”.`);
}
async function removeFromPlaylist(plId, id) {
  const pl = state.playlists.get(plId);
  if (!pl) return;
  pl.trackIds = pl.trackIds.filter((x) => x !== id);
  await db.put('playlists', pl);
  render();
}
async function moveInPlaylist(plId, from, to) {
  const pl = state.playlists.get(plId);
  if (!pl || from === to || to < 0 || to >= pl.trackIds.length) return;
  const [m] = pl.trackIds.splice(from, 1);
  pl.trackIds.splice(to, 0, m);
  await db.put('playlists', pl);
  render();
}
async function toggleLove() {
  const t = currentTrack();
  if (!t) return;
  let pl = state.playlists.get('loved');
  if (!pl) { pl = { id: 'loved', name: 'Loved Songs', trackIds: [], createdAt: 0, system: true }; state.playlists.set('loved', pl); }
  const has = pl.trackIds.includes(t.id);
  pl.trackIds = has ? pl.trackIds.filter((x) => x !== t.id) : [...pl.trackIds, t.id];
  await db.put('playlists', pl);
  updateNowPlaying();
  render();
  toast(has ? 'Removed from Loved Songs.' : 'Added to Loved Songs.');
}
async function deleteTrackFlow(id, title) {
  const where = isApp ? 'phone' : 'computer';
  if (!(await confirmDialog(`Remove “${title}”?`, `It will be removed from your library and every playlist. The original file on your ${where} isn’t touched.`, 'Remove'))) return;
  const gone = state.tracks.get(id);
  if (gone && gone.uri) {
    if (gone.folder) { excluded.add(gone.uri); db.put('meta', [...excluded], 'excluded'); } // don't bring it back on the next folder scan
    else releaseUri(gone.uri);
  }
  if (state.currentId === id) closeNowPlaying();
  await forgetTrack(id);
  await updateNowPlaying();
  updatePlayState(); render(); renderQueue(); saveSession();
}

// ---------- import ----------
const AUDIO_EXT = /\.(flac|m4a|alac|aac|mp4|aif|aiff|aifc|wav|wave|mp3|ogg|oga|opus)$/i;
async function makeThumb(blob, size = 240) {
  try {
    const bmp = await createImageBitmap(blob);
    const c = document.createElement('canvas');
    c.width = c.height = size;
    const s = Math.min(bmp.width, bmp.height);
    c.getContext('2d').drawImage(bmp, (bmp.width - s) / 2, (bmp.height - s) / 2, s, s, 0, 0, size, size);
    if (bmp.close) bmp.close();
    return await new Promise((r) => c.toBlob(r, 'image/jpeg', 0.86));
  } catch { return null; }
}
async function filesFromDataTransfer(dt) {
  const entries = [...(dt.items || [])].map((it) => (it.webkitGetAsEntry ? it.webkitGetAsEntry() : null)).filter(Boolean);
  if (!entries.length) return [...dt.files];
  const out = [];
  const walk = async (entry) => {
    if (entry.isFile) out.push(await new Promise((res, rej) => entry.file(res, rej)));
    else if (entry.isDirectory) {
      const reader = entry.createReader();
      let batch;
      do { batch = await new Promise((res, rej) => reader.readEntries(res, rej)); for (const e of batch) await walk(e); } while (batch.length);
    }
  };
  for (const e of entries) await walk(e);
  return out;
}
let importing = false;
const excluded = new Set();   // app: songs removed from a saved folder, skipped on rescans
const folders = new Map();    // app: saved folders, uri -> {uri, name, addedAt}

const META_VERSION = 2; // v1 app builds couldn't read tags past the start of a file on the phone

/** Read one song's tags/art and add it. `file` is a File (browser) or RemoteFile (app). */
async function addSong(file, extra, order) {
  const meta = await readMetadata(file);
  const id = uid();
  const artBlob = meta.art ? new Blob([meta.art.data], { type: meta.art.mime }) : null;
  const th = artBlob ? await makeThumb(artBlob) : null;
  // The app keeps a 640px copy of the cover rather than the full-size original, to save space.
  const art = artBlob && th ? (db.persistent ? (await makeThumb(artBlob, 640)) || artBlob : artBlob) : null;
  const t = {
    id, fileName: file.name, size: file.size, mime: file.type || '', addedAt: Date.now() + order,
    title: meta.title || stripExt(file.name), artist: meta.artist, album: meta.album, albumArtist: meta.albumArtist,
    year: meta.year, genre: meta.genre, trackNo: meta.trackNo, discNo: meta.discNo,
    codec: meta.codec, lossless: meta.lossless, sampleRate: meta.sampleRate, bitDepth: meta.bitDepth,
    channels: meta.channels, bitrate: meta.bitrate, duration: meta.duration,
    hasArt: !!art, analysis: null, metaVersion: META_VERSION, ...extra,
  };
  await db.addTrack(t, file instanceof RemoteFile ? null : file, art, th);
  state.tracks.set(id, t);
  if (th) thumbUrls.set(id, URL.createObjectURL(th));
  return id;
}

/** sources: [{ file, extra }] */
async function importSources(sources, { quiet = false } = {}) {
  if (importing) { toast('Still adding the previous songs…'); return []; }
  if (!sources.length) { if (!quiet) toast('No audio files found.'); return []; }
  importing = true;
  const seenUri = new Set([...state.tracks.values()].map((t) => t.uri).filter(Boolean));
  const seen = new Set([...state.tracks.values()].map((t) => `${t.fileName}|${t.size}`));
  const newIds = [];
  let skipped = 0, failed = 0, full = false;
  for (let i = 0; i < sources.length; i++) {
    const { file, extra } = sources[i];
    if (!quiet || sources.length > 3) toast(`Adding ${i + 1} of ${sources.length}…`, { sticky: true });
    if ((extra && extra.uri && seenUri.has(extra.uri)) || seen.has(`${file.name}|${file.size}`)) { skipped++; continue; }
    try {
      if (file instanceof RemoteFile) await file.ensureSize();
      const id = await addSong(file, extra, i);
      seen.add(`${file.name}|${file.size}`);
      if (extra && extra.uri) seenUri.add(extra.uri);
      newIds.push(id);
    } catch (e) {
      if (e && e.name === 'QuotaExceededError') { full = true; break; }
      failed++;
      console.warn('Import failed for', file.name, e);
    }
    // Keep the screen responsive while adding a big folder.
    if (i % 8 === 7) { render(); await new Promise((r) => setTimeout(r, 0)); }
  }
  importing = false;
  const page = currentPage();
  if (page && page.type === 'playlist' && newIds.length && !quiet) await addToPlaylist(page.key, newIds, { silent: true });
  db.requestPersistence();
  render();
  scheduleAnalysis(newIds);
  const parts = [];
  if (newIds.length) parts.push(`Added ${plural(newIds.length, 'song')}`);
  if (skipped && !quiet) parts.push(`${skipped} already in your library`);
  if (failed) parts.push(`${failed} couldn’t be read`);
  if (full) parts.push('storage is full');
  if (parts.length || !quiet) toast(parts.join(' · ') || 'Nothing added.');
  return newIds;
}

async function importFiles(list) {
  const files = [...list].filter((f) => AUDIO_EXT.test(f.name) || (f.type && f.type.startsWith('audio/')));
  return importSources(files.map((f) => ({ file: f, extra: {} })));
}

// ---------- app: songs on the phone ----------
const remoteSource = (it, folderUri) => ({
  file: new RemoteFile(it.uri, it.name, it.size, it.mime),
  extra: { uri: it.uri, folder: folderUri || null, path: it.path || '' },
});
const isAudioItem = (it) => AUDIO_EXT.test(it.name || '') || /^audio\//.test(it.mime || '');

async function addFilesFromPhone() {
  const r = await pickFiles();
  if (r.cancelled) return;
  if (r.error) { toast(`Couldn’t open the picker: ${r.error}`); return; }
  const items = (r.items || []).filter(isAudioItem);
  const temp = items.filter((it) => it.persisted === false);
  await importSources(items.map((it) => {
    const src = remoteSource(it, null);
    if (it.persisted === false) src.extra.temporary = true;
    return src;
  }));
  if (temp.length) {
    toast(`${plural(temp.length, 'song')} can only stay until you close Groove — the app you picked ${temp.length === 1 ? 'it' : 'them'} from doesn’t allow more. Pick from your phone’s storage or add the folder instead.`, { ms: 9000 });
    return;
  }
  if (r.grantLimit && r.grants >= r.grantLimit - 8) {
    toast(`Android lets an app remember about ${r.grantLimit} single songs. For a big library, use “Add a folder” instead.`, { ms: 8000 });
  }
}

async function addFolderFromPhone() {
  const r = await pickFolder();
  if (r.cancelled) return;
  if (r.error) { toast(`Couldn’t read that folder: ${r.error}`); return; }
  const f = { uri: r.folder.uri, name: r.folder.name || 'Music', addedAt: Date.now() };
  folders.set(f.uri, f);
  await db.put('folders', f);
  const items = (r.items || []).filter(isAudioItem);
  if (!items.length) { toast(`No songs found in “${f.name}”.`); return; }
  toast(`Found ${plural(items.length, 'song')} in “${f.name}”…`, { sticky: true });
  await importSources(items.filter((it) => !excluded.has(it.uri)).map((it) => remoteSource(it, f.uri)));
}

/** On launch: pick up songs added to saved folders, and drop ones that were deleted. */
async function syncFolders() {
  if (!isApp || !folders.size) return;
  const r = await rescanFolders([...folders.keys()]);
  if (r.error || !r.folders) return;
  const toAdd = [];
  const removed = [];
  for (const f of r.folders) {
    if (!f.ok) { console.warn('Folder not readable', f.uri, f.error); continue; } // SD card out, etc.: keep songs
    const present = new Set(f.items.map((it) => it.uri));
    for (const t of state.tracks.values()) if (t.folder === f.uri && !present.has(t.uri)) removed.push(t.id);
    const known = new Set([...state.tracks.values()].map((t) => t.uri));
    for (const it of f.items) if (!known.has(it.uri) && !excluded.has(it.uri) && isAudioItem(it)) toAdd.push(remoteSource(it, f.uri));
  }
  for (const id of removed) await forgetTrack(id);
  if (removed.length) { render(); renderQueue(); }
  if (toAdd.length) await importSources(toAdd, { quiet: true });
}

/** Re-read tags and covers for songs saved by an older app version (keeps analysis and playlists). */
async function refreshOldMetadata() {
  const old = [...state.tracks.values()].filter((t) => t.uri && (t.metaVersion || 1) < META_VERSION);
  let fixed = 0;
  for (const t of old) {
    try {
      const file = new RemoteFile(t.uri, t.fileName, t.size, t.mime);
      const meta = await readMetadata(file);
      const artBlob = meta.art ? new Blob([meta.art.data], { type: meta.art.mime }) : null;
      const th = artBlob ? await makeThumb(artBlob) : null;
      const art = th ? (await makeThumb(artBlob, 640)) || artBlob : null;
      for (const k of ['artist', 'album', 'albumArtist', 'year', 'genre', 'trackNo', 'discNo', 'codec', 'lossless', 'sampleRate', 'bitDepth', 'channels', 'bitrate']) t[k] = meta[k];
      t.title = meta.title || stripExt(t.fileName);
      if (meta.duration) t.duration = meta.duration;
      t.hasArt = !!art;
      t.metaVersion = META_VERSION;
      await db.put('tracks', t);
      if (art) {
        await db.put('art', art, t.id); await db.put('thumbs', th, t.id);
        const u = thumbUrls.get(t.id); if (u) URL.revokeObjectURL(u);
        thumbUrls.set(t.id, URL.createObjectURL(th));
      }
      fixed++;
      if (fixed % 10 === 0) render();
    } catch (e) { console.warn('Could not re-read', t.fileName, e); }
  }
  if (fixed) {
    render(); renderQueue();
    if (state.currentId && old.some((t) => t.id === state.currentId)) updateNowPlaying();
  }
}

/** Drop a song from the library without asking (its file is gone or its folder was removed). */
async function forgetTrack(id) {
  if (engine.other.trackId === id) engine.cancelMix();
  if (state.currentId === id) { engine.stop(); state.currentId = null; }
  const qi = state.queue.indexOf(id);
  state.queue = state.queue.filter((x) => x !== id);
  if (state.original) state.original = state.original.filter((x) => x !== id);
  if (qi >= 0 && qi < state.index) state.index--;
  state.index = Math.min(state.index, state.queue.length - 1);
  await db.deleteTrack(id);
  state.tracks.delete(id);
  for (const pl of state.playlists.values()) pl.trackIds = pl.trackIds.filter((x) => x !== id);
  const u = thumbUrls.get(id);
  if (u) { URL.revokeObjectURL(u); thumbUrls.delete(id); }
  const ai = analysisQueue.indexOf(id);
  if (ai >= 0) analysisQueue.splice(ai, 1);
}

async function removeFolderFlow(uri) {
  const f = folders.get(uri);
  if (!f) return;
  const ids = [...state.tracks.values()].filter((t) => t.folder === uri).map((t) => t.id);
  if (!(await confirmDialog(`Remove “${f.name}”?`, `Its ${plural(ids.length, 'song')} will be removed from Groove. The files on your phone aren’t touched.`, 'Remove'))) return;
  for (const id of ids) await forgetTrack(id);
  for (const u of [...excluded]) if (u.startsWith(`${uri}/`)) excluded.delete(u);
  db.put('meta', [...excluded], 'excluded');
  folders.delete(uri);
  await db.del('folders', uri);
  releaseUri(uri);
  await updateNowPlaying();
  updatePlayState(); render(); renderQueue(); saveSession(); renderLibrarySettings();
}

async function clearLibraryFlow() {
  if (!(await confirmDialog('Clear your library?', 'Every song, playlist and folder will be removed from Groove. Your music files aren’t touched.', 'Clear'))) return;
  engine.cancelMix(); engine.stop();
  for (const t of state.tracks.values()) if (t.uri && !t.folder) releaseUri(t.uri);
  for (const uri of folders.keys()) releaseUri(uri);
  for (const u of thumbUrls.values()) URL.revokeObjectURL(u);
  thumbUrls.clear(); folders.clear(); excluded.clear();
  state.tracks.clear(); state.playlists.clear();
  state.queue = []; state.original = null; state.index = -1; state.currentId = null;
  analysisQueue.length = 0;
  await db.clearLibrary();
  await updateNowPlaying();
  updatePlayState(); render(); renderQueue(); saveSession(); renderLibrarySettings();
}

function renderLibrarySettings() {
  const box = $('libraryBlock');
  if (!box) return;
  box.hidden = !isApp;
  if (!isApp) return;
  const list = $('folderList');
  const rows = [...folders.values()].sort((a, b) => a.name.localeCompare(b.name)).map((f) => {
    const n = [...state.tracks.values()].filter((t) => t.folder === f.uri).length;
    return h('li', { class: 'folder-row' },
      icon('folder'),
      h('span', { class: 'folder-text' }, h('b', { text: f.name }), h('small', { text: plural(n, 'song') })),
      h('button', { type: 'button', class: 'circle-btn small', 'aria-label': `Remove ${f.name}`, title: 'Remove folder', onclick: () => removeFolderFlow(f.uri) }, icon('x')));
  });
  const singles = [...state.tracks.values()].filter((t) => t.uri && !t.folder).length;
  if (singles) rows.push(h('li', { class: 'folder-row' }, icon('note'), h('span', { class: 'folder-text' }, h('b', { text: 'Single songs' }), h('small', { text: plural(singles, 'song') }))));
  if (!rows.length) rows.push(h('li', { class: 'folder-row empty', text: 'No music added yet.' }));
  list.replaceChildren(...rows);
}

// ---------- analysis queue ----------
function scheduleAnalysis(ids) {
  for (const id of ids) {
    const t = state.tracks.get(id);
    if (t && !t.analysis && !analysisQueue.includes(id) && analyzing !== id) analysisQueue.push(id);
  }
  for (const id of ids) refreshRow(id);
  pump();
}
function prioritizeAnalysis(id) {
  const t = state.tracks.get(id);
  if (!t || t.analysis || analyzing === id) return;
  const i = analysisQueue.indexOf(id);
  if (i >= 0) analysisQueue.splice(i, 1);
  analysisQueue.unshift(id);
  pump();
}
async function pump() {
  if (analyzing || !analysisQueue.length) return;
  const id = analysisQueue.shift();
  const t = state.tracks.get(id);
  if (!t || t.analysis) { pump(); return; }
  analyzing = id;
  try {
    let res;
    if (t.uri) {
      // On the phone, a full decode of a very long file (DJ mixes, albums in one file) can use more
      // memory than the WebView allows, so those are skipped instead of risking a crash.
      const long = (t.duration && t.duration > 15 * 60) || (!t.duration && t.size > 120e6);
      if (long) throw new Error('Too long to analyse on the phone');
      res = await analyzeBlob(new RemoteFile(t.uri, t.fileName, t.size, t.mime));
    } else {
      res = await analyzeBlob(await db.get('files', id));
    }
    t.analysis = res;
    if (!t.duration && res.duration) t.duration = res.duration;
  } catch (e) {
    console.warn('Analysis failed for', titleOf(t), e);
    t.analysis = { version: 1, error: String((e && e.message) || e) };
  }
  analyzing = null;
  if (state.tracks.has(id)) {
    await db.put('tracks', t).catch((e) => console.warn(e));
    refreshRow(id);
    if (id === state.currentId) updateAnalysisUI();
    if (mixUI && (id === state.currentId || id === engine.other.trackId)) refreshMixDecks();
    if (mixUI) renderFits();
    renderQueue();
  }
  // Give playback and the screen room to breathe between songs (more so on the phone).
  setTimeout(pump, isApp ? (engine.active.playing ? 600 : 150) : 30);
}
async function adjustBpm(factor) {
  const t = currentTrack();
  if (!t) return;
  if (factor === null) delete t.bpmUser;
  else { const b = bpmOf(t); if (!b) return; t.bpmUser = Math.round(b * factor * 100) / 100; }
  await db.put('tracks', t);
  updateAnalysisUI(); refreshRow(t.id); renderQueue(); refreshMix();
}

// =====================================================================
// Settings & session
// =====================================================================
function saveSettings() { try { localStorage.setItem('groove.settings', JSON.stringify(settings)); } catch { /* ignore */ } }
function loadSettings() {
  try {
    // Settings saved under the old name (Aura) carry over.
    const saved = localStorage.getItem('groove.settings') || localStorage.getItem('aura.settings') || '{}';
    Object.assign(settings, JSON.parse(saved));
    localStorage.removeItem('aura.settings'); localStorage.removeItem('aura.session');
  } catch { /* ignore */ }
  settings.crossfade = Math.min(12, Math.max(0, +settings.crossfade || 0));
  if (![4, 8, 16, 32].includes(settings.mixLen)) settings.mixLen = 8;
  if (!Array.isArray(settings.eqGains) || settings.eqGains.length !== 10) settings.eqGains = PRESETS.flat.gains.slice();
  settings.eqGains = settings.eqGains.map((g) => Math.min(12, Math.max(-12, +g || 0)));
  if (!PRESETS[settings.eqPreset] && settings.eqPreset !== 'custom') settings.eqPreset = 'flat';
  if (!STAGES[settings.stage]) settings.stage = 'off';
  settings.stageIntensity = Math.min(1, Math.max(0, +settings.stageIntensity || 0));
  engine.syncEnabled = settings.beatmatch;
  engine.cueSink = engine.sinkSupported ? settings.cueSink || '' : '';
}
function syncSettingsUI() {
  const r = $('xfadeRange');
  r.value = settings.crossfade; fill(r);
  $('xfadeValue').textContent = settings.crossfade ? `${settings.crossfade} s` : 'Off';
  $('beatmatchToggle').checked = settings.beatmatch;
  $('fadeNote').hidden = engine.volumeSupported;
  $('cueOutputBlock').hidden = !engine.sinkSupported || isApp;
  renderLibrarySettings();
  $('versionNote').textContent = `Groove ${APP_VERSION}${isApp ? ' for Android' : ''}`;
  renderQueue();
}
async function listOutputs() {
  const sel = $('cueOutput');
  try {
    const devs = (await navigator.mediaDevices.enumerateDevices()).filter((d) => d.kind === 'audiooutput' && d.deviceId && d.deviceId !== 'default');
    sel.replaceChildren(h('option', { value: '', text: 'Same as speakers' }), ...devs.map((d, i) => h('option', { value: d.deviceId, text: d.label || `Audio output ${i + 1}` })));
    sel.value = devs.some((d) => d.deviceId === settings.cueSink) ? settings.cueSink : '';
    $('btnDeviceNames').hidden = !devs.length || devs.every((d) => d.label);
  } catch { /* not available */ }
}
function saveSession() {
  try {
    localStorage.setItem('groove.session', JSON.stringify({
      currentId: state.currentId, queue: state.queue, original: state.original, index: state.index,
      shuffle: state.shuffle, repeat: state.repeat, position: engine.active.time || 0, tab: state.tab, context: state.context,
    }));
  } catch { /* storage unavailable */ }
}
function restoreSession() {
  let s = null;
  try { s = JSON.parse(localStorage.getItem('groove.session') || 'null'); } catch { s = null; }
  if (!s) return;
  const ok = (id) => state.tracks.has(id);
  state.shuffle = !!s.shuffle;
  state.repeat = ['off', 'all', 'one'].includes(s.repeat) ? s.repeat : 'off';
  if (['songs', 'artists', 'albums', 'playlists', 'mix'].includes(s.tab)) state.tab = s.tab;
  if (s.context) state.context = s.context;
  if (s.currentId && ok(s.currentId)) {
    state.queue = (s.queue || []).filter(ok);
    state.original = s.original ? s.original.filter(ok) : null;
    state.index = Math.max(0, state.queue.indexOf(s.currentId));
    if (!state.queue.length) { state.queue = [s.currentId]; state.index = 0; }
    loadTrack(s.currentId, { autoplay: false, startAt: s.position || 0 });
  }
}
function setVolume(v) {
  settings.volume = Math.min(1, Math.max(0, v));
  engine.setVolume(settings.volume);
  const r = $('npVolume');
  r.value = settings.volume; fill(r);
  saveSettings();
}

// =====================================================================
// Sound: equaliser + sound stage
// =====================================================================
const STAGE_ICONS = { off: 'stereo', spatial: '3d', headphones: 'headphones', speakers: 'speaker' };

function applySound() {
  if (!engine.enableGraph()) { toast('This browser can’t run the equaliser.'); return; }
  engine.fx.setEq(settings.eqOn, settings.eqGains);
  engine.fx.setStage(settings.stage, settings.stageIntensity);
  if (engine.ctx.state === 'suspended' && engine.active.playing) engine.ctx.resume();
}
function soundLabel() {
  const parts = [];
  if (settings.eqOn && settings.eqGains.some((g) => g !== 0)) parts.push(settings.eqPreset === 'custom' ? 'Custom EQ' : PRESETS[settings.eqPreset].name);
  if (settings.stage !== 'off') parts.push(STAGES[settings.stage].name);
  return parts.length ? parts.join(' · ') : 'Sound: original';
}
function syncSoundBadges() {
  const on = soundWanted();
  $('npSoundText').textContent = soundLabel();
  $('npSound').classList.toggle('on', on);
  $('btnSound').classList.toggle('on', on);
}
function renderSoundDialog() {
  $('eqToggle').checked = settings.eqOn;
  $('presetRow').replaceChildren(...Object.entries(PRESETS).map(([k, p]) => h('button', { type: 'button', 'aria-pressed': String(settings.eqPreset === k), dataset: { k }, text: p.name })),
    ...(settings.eqPreset === 'custom' ? [h('button', { type: 'button', 'aria-pressed': 'true', text: 'Custom' })] : []));
  const bands = $('eqBands');
  bands.classList.toggle('off', !settings.eqOn);
  bands.replaceChildren(...EQ_FREQS.map((f, i) => {
    const val = h('span', { class: 'eq-db' });
    const input = h('input', { type: 'range', min: '-12', max: '12', step: '0.5', value: String(settings.eqGains[i]), 'aria-label': `${EQ_LABELS[i]} hertz band`, dataset: { i: String(i) } });
    const show = () => { const g = +input.value; val.textContent = g === 0 ? '0' : `${g > 0 ? '+' : ''}${g}`; };
    show();
    input.addEventListener('input', () => {
      show();
      settings.eqGains[i] = +input.value;
      settings.eqOn = true; $('eqToggle').checked = true; bands.classList.remove('off');
      if (settings.eqPreset !== 'custom') { settings.eqPreset = 'custom'; renderPresetsOnly(); }
      soundChanged();
    });
    return h('div', { class: 'eq-band' }, val, h('div', { class: 'eq-slot' }, input), h('span', { class: 'eq-hz', text: EQ_LABELS[i] }));
  }));
  $('stageGrid').replaceChildren(...Object.entries(STAGES).map(([k, st]) => h('button', { type: 'button', 'aria-pressed': String(settings.stage === k), dataset: { k } }, icon(STAGE_ICONS[k]), st.name)));
  $('stageDesc').textContent = STAGES[settings.stage].desc;
  const r = $('stageIntensity');
  r.value = Math.round(settings.stageIntensity * 100); fill(r);
  $('stageIntVal').textContent = `${r.value}%`;
  $('intensityRow').hidden = r.hidden = settings.stage === 'off';
  updateHeadroomNote();
}
function renderPresetsOnly() {
  $('presetRow').querySelectorAll('button').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.k === settings.eqPreset)));
  if (settings.eqPreset === 'custom' && !$('presetRow').querySelector('button:not([data-k])')) $('presetRow').append(h('button', { type: 'button', 'aria-pressed': 'true', text: 'Custom' }));
}
function updateHeadroomNote() {
  const fx = engine.fx;
  const peak = fx && settings.eqOn ? fx.peakDb() : 0;
  $('eqHeadroom').textContent = settings.eqOn && peak > 0.05 ? `Level lowered by ${peak.toFixed(1)} dB to make room for boosts.` : '';
}
let soundSaveTimer = null;
function soundChanged() {
  applySound();
  syncSoundBadges();
  updateHeadroomNote();
  clearTimeout(soundSaveTimer);
  soundSaveTimer = setTimeout(saveSettings, 250);
}
function bindSound() {
  const open = () => { renderSoundDialog(); $('dlgSound').showModal(); };
  $('btnSound').addEventListener('click', open);
  $('npSound').addEventListener('click', open);
  $('eqToggle').addEventListener('change', (e) => { settings.eqOn = e.target.checked; $('eqBands').classList.toggle('off', !settings.eqOn); soundChanged(); });
  $('presetRow').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-k]'); if (!b) return;
    settings.eqPreset = b.dataset.k;
    settings.eqGains = PRESETS[b.dataset.k].gains.slice();
    settings.eqOn = b.dataset.k !== 'flat' || settings.eqOn;
    renderSoundDialog();
    soundChanged();
  });
  $('eqReset').addEventListener('click', () => { settings.eqPreset = 'flat'; settings.eqGains = PRESETS.flat.gains.slice(); renderSoundDialog(); soundChanged(); });
  $('stageGrid').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-k]'); if (!b) return;
    settings.stage = b.dataset.k;
    renderSoundDialog();
    soundChanged();
  });
  $('stageIntensity').addEventListener('input', (e) => {
    settings.stageIntensity = +e.target.value / 100;
    fill(e.target);
    $('stageIntVal').textContent = `${e.target.value}%`;
    soundChanged();
  });
}

// =====================================================================
// Wiring
// =====================================================================
const addFiles = () => (isApp ? addFilesFromPhone() : $('fileInput').click());
const addFolder = () => (isApp ? addFolderFromPhone() : $('folderInput').click());
function addItems() {
  return [
    { label: 'Add a folder…', icon: 'folder', action: addFolder },
    { label: 'Add songs…', icon: 'upload', action: addFiles },
  ];
}
function bind() {
  document.querySelectorAll('.tab').forEach((b) => b.addEventListener('click', () => setTab(b.dataset.tab)));
  $('btnBack').addEventListener('click', goBack);
  $('btnHome').addEventListener('click', () => setTab('songs'));

  $('btnSearch').addEventListener('click', () => {
    const f = $('searchField');
    f.hidden = !f.hidden;
    $('mainPanel').classList.toggle('search-open', !f.hidden);
    $('btnSearch').setAttribute('aria-pressed', String(!f.hidden));
    if (f.hidden) { $('search').value = ''; state.search = ''; render(); } else $('search').focus();
  });
  let st = null;
  $('search').addEventListener('input', (e) => { clearTimeout(st); st = setTimeout(() => { state.search = e.target.value; render(); }, 90); });
  $('search').addEventListener('keydown', (e) => { if (e.key === 'Escape') { e.preventDefault(); $('btnSearch').click(); } });

  const ctxName = () => (currentPage() ? $('viewTitle').textContent : 'Your library');
  $('btnPlayAll').addEventListener('click', () => playFromList(state.listIds, null, { shuffle: false, context: ctxName() }));
  $('btnShuffleAll').addEventListener('click', () => playFromList(state.listIds, null, { shuffle: true, context: ctxName() }));
  $('btnSort').addEventListener('click', (e) => openMenu(e.currentTarget, Object.entries(SORTS).map(([k, v]) => ({ label: v.label, checked: state.sort === k, action: () => { state.sort = k; render(); } }))));
  $('btnViewMore').addEventListener('click', (e) => {
    const p = currentPage();
    openMenu(e.currentTarget, p && p.type === 'playlist' ? playlistMenuItems(p.key) : addItems());
  });

  $('btnAdd').addEventListener('click', (e) => openMenu(e.currentTarget, [
    ...addItems(),
    { label: 'New playlist…', icon: 'playlist', action: () => newPlaylistFlow() },
  ]));
  $('btnEmptyAdd').addEventListener('click', addFiles);
  $('btnEmptyFolder').addEventListener('click', addFolder);
  $('btnAddFolderSettings').addEventListener('click', addFolder);
  $('btnClearLibrary').addEventListener('click', clearLibraryFlow);
  for (const inp of [$('fileInput'), $('folderInput')]) inp.addEventListener('change', () => { if (inp.files.length) importFiles(inp.files); inp.value = ''; });
  let dragDepth = 0;
  const hasFiles = (e) => e.dataTransfer && [...e.dataTransfer.types].includes('Files');
  window.addEventListener('dragenter', (e) => { if (!hasFiles(e)) return; e.preventDefault(); dragDepth++; $('dropOverlay').classList.add('active'); });
  window.addEventListener('dragover', (e) => { if (!hasFiles(e)) return; e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; });
  window.addEventListener('dragleave', (e) => { if (!hasFiles(e)) return; if (--dragDepth <= 0) { dragDepth = 0; $('dropOverlay').classList.remove('active'); } });
  window.addEventListener('drop', async (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault(); dragDepth = 0; $('dropOverlay').classList.remove('active');
    importFiles(await filesFromDataTransfer(e.dataTransfer));
  });

  $('npPlay').addEventListener('click', togglePlay);
  $('miniPlay').addEventListener('click', togglePlay);
  $('npNext').addEventListener('click', () => next());
  $('miniNext').addEventListener('click', () => next());
  $('npPrev').addEventListener('click', prev);
  $('npShuffle').addEventListener('click', toggleShuffle);
  $('npRepeat').addEventListener('click', cycleRepeat);
  $('npVolume').addEventListener('input', (e) => setVolume(+e.target.value));
  if (!engine.volumeSupported) $('npVolumeWrap').hidden = true;
  $('npLove').addEventListener('click', toggleLove);
  $('miniOpen').addEventListener('click', openNowPlaying);
  $('npClose').addEventListener('click', closeNowPlaying);
  $('deckZoomIn').addEventListener('click', () => npDeck.zoom(1 / 1.5));
  $('deckZoomOut').addEventListener('click', () => npDeck.zoom(1.5));
  $('npMore').addEventListener('click', (e) => {
    const id = state.currentId;
    if (!id) return;
    const t = currentTrack();
    openMenu(e.currentTarget, [
      { label: 'Add to playlist…', icon: 'playlist', action: () => pickPlaylistFor([id]) },
      { label: `Go to ${groupArtistOf(t)}`, icon: 'note', action: () => { closeNowPlaying(); state.stack = []; openPage({ type: 'artist', key: groupArtistOf(t) }); } },
      { label: 'Open Mix', icon: 'mix', action: () => { closeNowPlaying(); setTab('mix'); } },
      { label: 'Get info', icon: 'info', action: () => showInfo(id) },
    ]);
  });
  $('npBpm').addEventListener('click', (e) => {
    const t = currentTrack();
    if (!t || !bpmOf(t)) return;
    openMenu(e.currentTarget, [
      { label: `Double (${fmtBpm(bpmOf(t) * 2)} BPM)`, action: () => adjustBpm(2) },
      { label: `Halve (${fmtBpm(bpmOf(t) / 2)} BPM)`, action: () => adjustBpm(0.5) },
      t.bpmUser != null ? { label: `Reset to detected (${fmtBpm(t.analysis.bpm)} BPM)`, action: () => adjustBpm(null) } : null,
    ]);
  });

  const openSettings = () => { syncSettingsUI(); if (engine.sinkSupported) listOutputs(); $('dlgSettings').showModal(); };
  $('btnSettings').addEventListener('click', openSettings);
  $('btnXfadeQuick').addEventListener('click', openSettings);
  $('xfadeRange').addEventListener('input', (e) => {
    settings.crossfade = +e.target.value;
    if (settings.crossfade > 0) engine.enableFading();
    fill(e.target);
    $('xfadeValue').textContent = settings.crossfade ? `${settings.crossfade} s` : 'Off';
    saveSettings(); renderQueue();
  });
  $('beatmatchToggle').addEventListener('change', (e) => { settings.beatmatch = e.target.checked; saveSettings(); });
  $('cueOutput').addEventListener('change', (e) => { settings.cueSink = e.target.value; engine.setCueSink(settings.cueSink); saveSettings(); });
  $('btnDeviceNames').addEventListener('click', async () => {
    try {
      const s = await navigator.mediaDevices.getUserMedia({ audio: true });
      s.getTracks().forEach((t) => t.stop());
    } catch { toast('Device names need microphone permission. The outputs still work without names.'); }
    listOutputs();
  });
  $('dlgPromptCancel').addEventListener('click', () => $('dlgPrompt').close('cancel'));
  bindSound();

  engine.addEventListener('play', updatePlayState);
  engine.addEventListener('pause', (e) => { updatePlayState(); if (e.detail.active) saveSession(); });
  engine.addEventListener('ended', (e) => { if (e.detail.active && !engine.mixing) next(true); });
  engine.addEventListener('loadedmetadata', (e) => {
    if (!e.detail.active) return;
    const t = currentTrack();
    const d = engine.active.duration;
    if (t && d && (!t.duration || Math.abs(t.duration - d) > 1)) { t.duration = d; db.put('tracks', t).catch(() => {}); refreshRow(t.id); }
    npDeck.duration = npOverview.duration = currentDuration();
    npOverview.prerender();
    updatePosition();
    pushNative();
  });
  engine.addEventListener('seeked', (e) => { if (e.detail.active) { updatePosition(); pushNative(); } });
  engine.addEventListener('error', (e) => {
    const deck = e.detail.deck;
    const t = deck.trackId && state.tracks.get(deck.trackId);
    if (!t || !deck.el.getAttribute('src')) return;
    updatePlayState();
    if (t.uri) {
      // Was the file moved or deleted, or is it a format Android's WebView can't play?
      fetch(mediaUrl(t.uri), { headers: { Range: 'bytes=0-0' } }).then((r) => {
        if (r.status === 404) toast(`Can’t find “${titleOf(t)}” — it may have been moved or deleted.`);
        else if (r.status === 403) toast(`Groove lost permission to read “${titleOf(t)}”. Add it (or its folder) again.`);
        else {
          const why = (deck.el.error && deck.el.error.message) || `media error ${deck.el.error ? deck.el.error.code : '?'}`;
          if (t.convert && rescuing !== t.id) toast(`Android can’t play “${titleOf(t)}”, even converted (${why}).`, { ms: 8000 });
          else if (deck === engine.active) rescuePlayback(t, why);
          else { t.convert = true; db.put('tracks', t).catch(() => {}); } // next time it's cued it'll be converted
        }
      }).catch(() => {});
      return;
    }
    const hint = /aif|alac/i.test(t.codec || '') ? ' ALAC and AIFF play in Safari.' : '';
    toast(`This browser can’t play “${titleOf(t)}”.${hint}`);
  });
  let lastSave = 0;
  engine.addEventListener('timeupdate', (e) => { if (e.detail.active && Date.now() - lastSave > 5000) { lastSave = Date.now(); saveSession(); } });
  engine.addEventListener('swap', (e) => {
    const { id } = e.detail;
    if (state.queue[state.index + 1] === id) state.index++;
    else if (state.repeat === 'all' && state.index === state.queue.length - 1 && state.queue[0] === id) state.index = 0;
    else {
      state.queue.splice(state.index + 1, 0, id);
      if (state.original) state.original.splice(Math.max(0, state.original.indexOf(state.currentId)) + 1, 0, id);
      state.index++;
    }
    state.currentId = id;
    autoXfadeFor = null;
    syncModesUI();
    updateNowPlaying(); markCurrentRow(); renderQueue(); updatePlayState(); refreshMix(); saveSession();
  });
  window.addEventListener('pagehide', saveSession);
  document.addEventListener('visibilitychange', () => { if (document.hidden) saveSession(); });
  matchMedia('(min-width: 1181px)').addEventListener('change', () => { closeNowPlaying(); syncNpInert(); });

  if ('mediaSession' in navigator) {
    const ms = navigator.mediaSession;
    const set = (a, fn) => { try { ms.setActionHandler(a, fn); } catch { /* unsupported */ } };
    set('play', () => engine.play());
    set('pause', () => engine.pause());
    set('previoustrack', prev);
    set('nexttrack', () => next());
    set('seekto', (d) => seekTo(d.seekTime));
    set('seekbackward', (d) => seekTo(engine.active.time - (d.seekOffset || 10)));
    set('seekforward', (d) => seekTo(engine.active.time + (d.seekOffset || 10)));
  }

  document.addEventListener('keydown', (e) => {
    if (e.defaultPrevented || document.querySelector('dialog[open]')) return;
    const el = e.target, tag = el.tagName;
    const typing = (tag === 'INPUT' && el.type !== 'range') || tag === 'TEXTAREA' || tag === 'SELECT' || el.isContentEditable;
    if (e.key === 'Escape') { if (state.npOpen) closeNowPlaying(); else if (currentPage()) goBack(); return; }
    if (typing) return;
    if (e.code === 'Space' && tag !== 'BUTTON' && tag !== 'INPUT') { e.preventDefault(); togglePlay(); }
    else if ((e.metaKey || e.ctrlKey) && e.key === 'ArrowRight') { e.preventDefault(); next(); }
    else if ((e.metaKey || e.ctrlKey) && e.key === 'ArrowLeft') { e.preventDefault(); prev(); }
    else if (tag !== 'INPUT' && e.key === 'ArrowRight' && state.currentId) { e.preventDefault(); seekTo(engine.active.time + (e.shiftKey ? 30 : 5)); }
    else if (tag !== 'INPUT' && e.key === 'ArrowLeft' && state.currentId) { e.preventDefault(); seekTo(engine.active.time - (e.shiftKey ? 30 : 5)); }
    else if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'f') { e.preventDefault(); if ($('searchField').hidden) $('btnSearch').click(); else $('search').focus(); }
  });
}
function updatePosition() {
  if (!('mediaSession' in navigator) || !navigator.mediaSession.setPositionState) return;
  const d = currentDuration();
  if (!d) return;
  try { navigator.mediaSession.setPositionState({ duration: d, position: Math.min(engine.active.time, d), playbackRate: engine.active.el.playbackRate || 1 }); } catch { /* ignore */ }
}

function bindApp() {
  if (isApp) {
    document.body.classList.add('is-app');
    // On the phone a folder is the better way in: one permission covers every song in it.
    const folderBtn = $('btnEmptyFolder'), filesBtn = $('btnEmptyAdd');
    folderBtn.classList.replace('pill-outline', 'pill-yellow');
    filesBtn.classList.replace('pill-yellow', 'pill-outline');
    filesBtn.lastChild.textContent = 'Choose songs';
    folderBtn.after(filesBtn);
  }
  onRemote((cmd) => {
    if (cmd === 'play') { if (state.currentId) engine.play().catch(() => {}); }
    else if (cmd === 'pause' || cmd === 'stop') engine.pause();
    else if (cmd === 'toggle') togglePlay();
    else if (cmd === 'next') next();
    else if (cmd === 'prev') prev();
    else if (cmd.startsWith('seek:')) seekTo(parseFloat(cmd.slice(5)) || 0);
  });
  onBack(() => {
    const dlg = document.querySelector('dialog[open]');
    if (dlg) { dlg.close(); return true; }
    if (!$('menu').hidden) { closeMenu(); return true; }
    if (state.npOpen) { closeNowPlaying(); return true; }
    if (currentPage()) { goBack(); return true; }
    if (!$('searchField').hidden) { $('btnSearch').click(); return true; }
    if (state.tab !== 'songs') { setTab('songs'); return true; }
    return false;
  });
  // Keep fades and auto-crossfade running while the screen is off or the app is in the background
  // (animation frames stop there; timers keep running while audio plays).
  setInterval(() => { if (document.hidden) { engine.tick(); maybeAutoCrossfade(); } }, 200);
  setInterval(checkStall, 1000);
}

async function init() {
  await db.purgeOldLibrary();
  bindApp();
  loadSettings();
  bind();
  syncNpInert();
  setVolume(settings.volume ?? 1);
  try {
    const [tracks, playlists, thumbs, savedFolders, ex] = await Promise.all([
      db.getAll('tracks'), db.getAll('playlists'), db.entries('thumbs'), db.getAll('folders'), db.get('meta', 'excluded'),
    ]);
    for (const t of tracks) {
      if (t.temporary) { db.deleteTrack(t.id); continue; } // permission ended when the app closed
      state.tracks.set(t.id, t);
    }
    for (const f of savedFolders) folders.set(f.uri, f);
    for (const u of ex || []) excluded.add(u);
    for (const p of playlists) { p.trackIds = (p.trackIds || []).filter((id) => state.tracks.has(id)); state.playlists.set(p.id, p); }
    for (const [id, blob] of thumbs) if (state.tracks.has(id)) thumbUrls.set(id, URL.createObjectURL(blob));
  } catch (e) {
    console.error(e);
    toast('Your library couldn’t be opened. Private browsing can block storage.');
  }
  restoreSession();
  syncModesUI();
  render();
  renderQueue();
  if (!state.currentId) updateNowPlaying();
  updatePlayState();
  const pending = [...state.tracks.values()].filter((t) => !t.analysis).map((t) => t.id);
  if (pending.length) scheduleAnalysis(pending);
  if (soundWanted()) applySound();
  syncSoundBadges();
  requestAnimationFrame(frame);
  hideSplash();
  syncFolders().catch((e) => console.warn('Folder scan failed', e)).then(() => isApp && refreshOldMetadata());
}

init();
