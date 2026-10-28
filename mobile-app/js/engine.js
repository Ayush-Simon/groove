// Two-deck playback engine.
// Deck = one <audio> element playing an original, unmodified file.
// Mixing only changes each deck's volume (and, when beat-syncing, its speed with pitch kept).

import { SoundFX } from './sound.js';

const HALF_PI = Math.PI / 2;
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));

function preservePitch(el) {
  el.preservesPitch = true;
  el.mozPreservesPitch = true;
  el.webkitPreservesPitch = true;
}

class Deck {
  constructor(name) {
    this.name = name;
    this.el = new Audio();
    this.el.preload = 'auto';
    preservePitch(this.el);
    this.trackId = null;
    this.url = null;
    this.gainNode = null;
    this.sink = '';
  }
  /** source: a File/Blob (played through a blob: URL) or a URL string (the app's stream of a file on the phone). */
  async load(id, source, startAt = 0) {
    this.unload();
    if (typeof source === 'string') this.url = null;
    else this.url = URL.createObjectURL(source);
    this.trackId = id;
    this.el.playbackRate = 1;
    this.el.src = this.url || source;
    await new Promise((resolve) => {
      const done = () => { this.el.removeEventListener('loadedmetadata', done); this.el.removeEventListener('error', done); resolve(); };
      this.el.addEventListener('loadedmetadata', done);
      this.el.addEventListener('error', done);
    });
    if (startAt > 0 && isFinite(this.el.duration)) this.el.currentTime = Math.min(startAt, Math.max(0, this.el.duration - 1));
  }
  unload() {
    this.el.pause();
    if (this.url) URL.revokeObjectURL(this.url);
    this.url = null;
    this.trackId = null;
    this.el.removeAttribute('src');
    this.el.load();
  }
  get playing() { return !this.el.paused && !!this.trackId; }
  get time() { return this.el.currentTime || 0; }
  get duration() { return isFinite(this.el.duration) ? this.el.duration : 0; }
  setLevel(v) {
    v = clamp(v, 0, 1);
    if (this.gainNode) this.gainNode.gain.value = v;
    else this.el.volume = v;
  }
}

export class Engine extends EventTarget {
  /**
   * @param {object} o
   * @param {(id:string)=>Promise<Blob|string>} o.getBlob  the file, or a URL to stream it from
   * @param {(id:string)=>({bpm:number, beatOffset:number, downbeat:number}|null)} o.getGrid
   */
  constructor({ getBlob, getGrid }) {
    super();
    this.getBlob = getBlob;
    this.getGrid = getGrid;
    this.decks = [new Deck('A'), new Deck('B')];
    this.activeIdx = 0;
    this.volume = 1;
    this.x = 0;            // crossfader: 0 = active deck only, 1 = other deck only
    this.mix = null;       // automated fade in progress
    this.sync = null;      // { mult, base } while the other deck is tempo-locked
    this.glide = null;     // tempo returning to 100 % after a mix
    this.cueListen = false;
    this.syncEnabled = true;
    this.cueLevel = 0.6;
    this.cueSink = '';
    this.pendingStart = null;
    this.ctx = null;
    this.lastTick = performance.now();

    const probe = new Audio();
    probe.volume = 0.5;
    this.volumeSupported = probe.volume === 0.5; // false on iPhone/iPad
    this.sinkCapable = 'setSinkId' in HTMLMediaElement.prototype && this.volumeSupported;
    this.fx = null;

    for (const deck of this.decks) {
      for (const type of ['play', 'pause', 'ended', 'loadedmetadata', 'seeked', 'error', 'timeupdate']) {
        deck.el.addEventListener(type, () => this.dispatchEvent(new CustomEvent(type, { detail: { deck, active: deck === this.active } })));
      }
      deck.el.addEventListener('ended', () => this.onDeckEnded(deck));
    }
    this.applyLevels();
  }

  get active() { return this.decks[this.activeIdx]; }
  get other() { return this.decks[1 - this.activeIdx]; }
  get canFade() { return this.volumeSupported || !!this.ctx; }
  // A separate preview output only works while audio isn't routed through the Web Audio mixer.
  get sinkSupported() { return this.sinkCapable && !this.ctx; }
  get mixing() { return !!this.mix || this.x > 0 || !!this.pendingStart; }

  /**
   * Route both decks through the Web Audio mixer (needed for the equaliser and sound stage,
   * and for fades on iPhone/iPad where <audio>.volume can't be changed). Call from a tap.
   */
  enableGraph() {
    if (this.ctx) return true;
    try {
      const AC = window.AudioContext || window.webkitAudioContext;
      this.ctx = new AC({ latencyHint: 'playback' });
      this.fx = new SoundFX(this.ctx);
      this.fx.output.connect(this.ctx.destination);
      for (const d of this.decks) {
        if (d.sink) { try { d.el.setSinkId(''); } catch { /* ignore */ } d.sink = ''; }
        d.el.volume = 1;
        const src = this.ctx.createMediaElementSource(d.el);
        d.gainNode = this.ctx.createGain();
        src.connect(d.gainNode).connect(this.fx.input);
      }
      this.cueSink = '';
      this.ctx.resume();
      this.applyLevels();
      this.dispatchEvent(new CustomEvent('graph'));
      return true;
    } catch (e) {
      console.warn('Could not start the audio mixer', e);
      this.ctx = null; this.fx = null;
      return false;
    }
  }

  /** Fades only need the mixer on devices that can't change <audio> volume. */
  enableFading() { return this.canFade || this.enableGraph(); }

  setVolume(v) { this.volume = clamp(v, 0, 1); this.applyLevels(); }

  applyLevels() {
    const a = Math.cos(this.x * HALF_PI);
    let b = Math.sin(this.x * HALF_PI);
    const other = this.other;
    const cueOnOwnDevice = this.cueSink && other.sink === this.cueSink;
    if (cueOnOwnDevice) b = this.x > 0 ? b : 1; // headphones hear the cue at full level
    else if (this.cueListen) b = Math.max(b, this.cueLevel * (1 - this.x));
    this.active.setLevel(a * this.volume);
    other.setLevel(b * (cueOnOwnDevice && this.x === 0 ? 1 : this.volume));
  }

  // ---------- main deck ----------
  async load(id, { autoplay = true, startAt = 0 } = {}) {
    this.cancelMix();
    const blob = await this.getBlob(id);
    if (!blob) throw new Error('missing file');
    await this.active.load(id, blob, startAt);
    this.applyLevels();
    if (autoplay) await this.play();
  }

  async play() {
    if (this.ctx && this.ctx.state === 'suspended') await this.ctx.resume();
    const jobs = [this.active.el.play()];
    if (this.mixing && this.other.trackId && !this.pendingStart) jobs.push(this.other.el.play());
    try { await Promise.all(jobs); } catch (e) { if (e.name !== 'AbortError') throw e; }
  }

  pause() {
    this.active.el.pause();
    if (this.mixing || this.other.playing) this.other.el.pause();
    if (this.pendingStart) { clearTimeout(this.pendingStart); this.pendingStart = null; }
  }

  seek(t) {
    const d = this.active.duration;
    if (!d) return;
    this.active.el.currentTime = clamp(t, 0, Math.max(0, d - 0.05));
  }

  stop() {
    this.cancelMix();
    this.active.unload();
  }

  // ---------- cue deck (the "other" deck) ----------
  async cue(id, { startAt = 0 } = {}) {
    this.cancelMix();
    const blob = await this.getBlob(id);
    if (!blob) throw new Error('missing file');
    await this.other.load(id, blob, startAt);
    if (this.cueSink) await this.routeOther(this.cueSink);
    this.applyLevels();
    this.dispatchEvent(new CustomEvent('cue', { detail: { id } }));
  }

  async cueToggle() {
    const o = this.other;
    if (!o.trackId) return;
    if (o.playing) { o.el.pause(); this.sync = null; return; }
    if (this.ctx && this.ctx.state === 'suspended') await this.ctx.resume();
    this.lockTempo();
    await o.el.play().catch((e) => console.warn(e));
  }

  cueSeek(t) {
    const o = this.other;
    if (!o.duration) return;
    o.el.currentTime = clamp(t, 0, o.duration - 0.05);
  }

  async setCueSink(deviceId) {
    this.cueSink = deviceId || '';
    if (this.other.trackId && this.x === 0) await this.routeOther(this.cueSink);
    this.applyLevels();
  }

  async routeOther(sink) {
    const o = this.other;
    if (!this.sinkSupported || o.sink === sink) return;
    try { await o.el.setSinkId(sink); o.sink = sink; } catch (e) { console.warn('setSinkId failed', e); }
  }

  /** Manual crossfader. x in 0..1. Returns true if the mix was completed. */
  async setCrossfader(x, { release = false } = {}) {
    this.mix = null;
    const o = this.other;
    if (!o.trackId) { this.x = 0; this.applyLevels(); return false; }
    this.x = clamp(x, 0, 1);
    if (this.x > 0) {
      await this.routeOther('');
      if (!o.playing && this.active.playing) { this.lockTempo(); o.el.play().catch(() => {}); }
    }
    this.applyLevels();
    if (release && this.x >= 0.999) { this.commit('manual'); return true; }
    return false;
  }

  // ---------- tempo sync ----------
  bestMultiplier(bpmA, bpmB) {
    let best = null;
    for (const m of [1, 2, 0.5]) {
      const r = (bpmA * m) / bpmB;
      if (Math.abs(r - 1) <= 0.08 && (!best || Math.abs(r - 1) < Math.abs(best.r - 1))) best = { m, r };
    }
    return best;
  }

  /** Match the other deck's tempo to the active deck (pitch preserved). */
  lockTempo() {
    const ga = this.getGrid(this.active.trackId), gb = this.getGrid(this.other.trackId);
    this.sync = null;
    this.other.el.playbackRate = 1;
    if (!this.syncEnabled || !ga || !gb) return false;
    const rateA = this.active.el.playbackRate || 1;
    const best = this.bestMultiplier(ga.bpm * rateA, gb.bpm);
    if (!best) return false;
    this.sync = { mult: best.m, base: best.r };
    this.other.el.playbackRate = best.r;
    return true;
  }

  beatPhase(deck, grid) {
    return (deck.time - grid.beatOffset) / (60 / grid.bpm);
  }

  /** Phase-lock loop: nudges the other deck's speed so its beats land on the active deck's beats. */
  phaseLock() {
    if (!this.sync || !this.other.playing || !this.active.playing) return;
    const ga = this.getGrid(this.active.trackId), gb = this.getGrid(this.other.trackId);
    if (!ga || !gb) return;
    const pa = this.beatPhase(this.active, ga) * this.sync.mult;
    const pb = this.beatPhase(this.other, gb);
    let e = pa - pb;
    e -= Math.round(e); // wrap to [-0.5, 0.5] beats
    const periodB = 60 / gb.bpm;
    const extra = clamp((e * periodB) / 2, -0.03, 0.03);
    const rate = this.sync.base * (1 + extra);
    if (Math.abs(this.other.el.playbackRate - rate) > 0.0005) this.other.el.playbackRate = rate;
    this.lastPhaseError = e;
  }

  nextBarTime(deck, grid, after) {
    const period = 60 / grid.bpm;
    const first = grid.beatOffset + (grid.downbeat || 0) * period;
    const bar = period * 4;
    const k = Math.ceil((after - first) / bar);
    return first + Math.max(0, k) * bar;
  }

  /**
   * Fade from the active deck to the other deck over `seconds`.
   * fromStart: start the other deck at its first bar (auto-crossfade); otherwise from its cue position.
   */
  async mixTo(seconds, { fromStart = false, kind = 'manual' } = {}) {
    const o = this.other;
    if (!o.trackId) return;
    if (!this.canFade) seconds = 0.05;
    await this.routeOther('');
    const ga = this.getGrid(this.active.trackId), gb = this.getGrid(o.trackId);
    const synced = this.lockTempo();

    const begin = () => {
      this.pendingStart = null;
      this.mix = { from: this.x, dur: Math.max(0.05, seconds), elapsed: 0, kind };
      if (!o.playing) o.el.play().catch((e) => console.warn(e));
      this.dispatchEvent(new CustomEvent('mixstart', { detail: { kind } }));
    };

    if (fromStart && gb && synced) {
      const startPos = this.nextBarTime(o, gb, 0);
      o.el.currentTime = startPos > 60 / gb.bpm * 4 ? 0 : startPos;
    } else if (fromStart) {
      o.el.currentTime = 0;
    } else if (gb && synced && !o.playing) {
      // Snap the cue point to the nearest bar so phrases line up.
      const bar = (60 / gb.bpm) * 4;
      const first = gb.beatOffset + (gb.downbeat || 0) * (60 / gb.bpm);
      const k = Math.round((o.time - first) / bar);
      o.el.currentTime = Math.max(0, first + k * bar);
    }

    if (synced && ga && this.active.playing && !o.playing) {
      const now = this.active.time;
      const rateA = this.active.el.playbackRate || 1;
      const target = this.nextBarTime(this.active, ga, now + 0.12);
      const wait = (target - now) / rateA;
      if (wait > 0 && wait < 4) { this.pendingStart = setTimeout(begin, wait * 1000); return; }
    }
    begin();
  }

  /** Called when the active track is about to end. */
  async autoCrossfade(nextId, seconds) {
    if (!this.canFade || this.mixing) return false;
    try {
      const blob = await this.getBlob(nextId);
      await this.other.load(nextId, blob, 0);
      await this.mixTo(seconds, { fromStart: true, kind: 'auto' });
      return true;
    } catch (e) {
      console.warn('Auto crossfade failed', e);
      return false;
    }
  }

  cancelMix() {
    if (this.pendingStart) { clearTimeout(this.pendingStart); this.pendingStart = null; }
    this.mix = null;
    this.sync = null;
    this.x = 0;
    if (this.other.trackId) this.other.unload();
    this.applyLevels();
  }

  /** Make the other deck the main one. */
  commit(kind) {
    const old = this.active;
    this.activeIdx = 1 - this.activeIdx;
    this.mix = null;
    this.x = 0;
    old.unload();
    const rate = this.active.el.playbackRate;
    this.glide = Math.abs(rate - 1) > 0.001 ? { from: rate, t: 0, dur: 10 } : null;
    this.sync = null;
    this.applyLevels();
    this.dispatchEvent(new CustomEvent('swap', { detail: { id: this.active.trackId, kind } }));
  }

  onDeckEnded(deck) {
    if (deck === this.active && this.mix && this.other.playing) {
      this.commit(this.mix.kind);
      return;
    }
    if (deck === this.other) { this.sync = null; if (this.x > 0.5) this.commit('manual'); }
  }

  /** Drive fades, phase lock and tempo glide. Call every animation frame. */
  tick() {
    const now = performance.now();
    const dt = Math.min(0.25, (now - this.lastTick) / 1000);
    this.lastTick = now;
    if (this.mix && (this.active.playing || this.other.playing)) {
      this.mix.elapsed += dt;
      const p = Math.min(1, this.mix.elapsed / this.mix.dur);
      this.x = this.mix.from + (1 - this.mix.from) * p;
      this.applyLevels();
      if (p >= 1) this.commit(this.mix.kind);
    }
    this.phaseLock();
    if (this.glide && this.active.playing) {
      this.glide.t += dt;
      const p = Math.min(1, this.glide.t / this.glide.dur);
      this.active.el.playbackRate = this.glide.from + (1 - this.glide.from) * p;
      if (p >= 1) this.glide = null;
    }
  }
}
