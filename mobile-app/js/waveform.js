// DJ-style waveforms drawn from the analysis data.
// DeckWaveform: zoomed, scrolling view with a fixed centre playhead and beat grid.
// OverviewWaveform: the whole track, doubles as the seek bar.

const COLORS = {
  low: '#3a6dff',   // bass  < 250 Hz
  mid: '#ff9a1f',   // mids  250 Hz – 4 kHz
  high: '#f4f4f6',  // highs > 4 kHz
};

function setupCanvas(canvas, onResize) {
  const ctx = canvas.getContext('2d');
  const state = { w: 0, h: 0, dpr: 1 };
  let ready = false;
  const resize = () => {
    const rect = canvas.getBoundingClientRect();
    const dpr = Math.min(window.devicePixelRatio || 1, 3);
    const w = Math.max(1, Math.round(rect.width * dpr));
    const h = Math.max(1, Math.round(rect.height * dpr));
    if (w !== canvas.width || h !== canvas.height) {
      canvas.width = w; canvas.height = h;
    }
    Object.assign(state, { w, h, dpr });
    if (ready) onResize();
  };
  resize();
  ready = true;
  new ResizeObserver(resize).observe(canvas);
  return { ctx, state };
}

function bandMax(arr, a, b) {
  let m = 0;
  const lo = Math.max(0, Math.floor(a)), hi = Math.min(arr.length - 1, Math.ceil(b));
  for (let i = lo; i <= hi; i++) if (arr[i] > m) m = arr[i];
  return m;
}

// Fill a mirrored band envelope given per-column amplitudes (0..1).
function fillMirrored(ctx, amps, centerY, halfH, color) {
  const n = amps.length;
  ctx.beginPath();
  ctx.moveTo(0, centerY);
  for (let x = 0; x < n; x++) ctx.lineTo(x, centerY - amps[x] * halfH);
  ctx.lineTo(n, centerY);
  for (let x = n - 1; x >= 0; x--) ctx.lineTo(x, centerY + amps[x] * halfH);
  ctx.closePath();
  ctx.fillStyle = color;
  ctx.fill();
}

export class DeckWaveform {
  constructor(canvas, { onScrub } = {}) {
    this.canvas = canvas;
    this.data = null;
    this.windowSec = 8;
    this.time = 0;
    this.duration = 0;
    this.onScrub = onScrub;
    this.dragging = null;
    const { ctx, state } = setupCanvas(canvas, () => this.render());
    this.ctx = ctx; this.s = state;
    this.bindPointer();
  }

  setData(analysis, duration) {
    this.data = analysis && analysis.low ? analysis : null;
    this.duration = duration || (analysis && analysis.duration) || 0;
    this.render();
  }

  zoom(factor) {
    this.windowSec = Math.min(32, Math.max(2, this.windowSec * factor));
    this.render();
  }

  bindPointer() {
    const c = this.canvas;
    c.addEventListener('pointerdown', (e) => {
      if (!this.data) return;
      c.setPointerCapture(e.pointerId);
      this.dragging = { x: e.clientX, t: this.time };
    });
    c.addEventListener('pointermove', (e) => {
      if (!this.dragging) return;
      const rect = c.getBoundingClientRect();
      const dt = ((this.dragging.x - e.clientX) / rect.width) * this.windowSec;
      const t = Math.min(this.duration, Math.max(0, this.dragging.t + dt));
      this.render(t);
      this.onScrub && this.onScrub(t, false);
    });
    const end = (e) => {
      if (!this.dragging) return;
      const rect = c.getBoundingClientRect();
      const dt = ((this.dragging.x - e.clientX) / rect.width) * this.windowSec;
      const t = Math.min(this.duration, Math.max(0, this.dragging.t + dt));
      this.dragging = null;
      this.onScrub && this.onScrub(t, true);
    };
    c.addEventListener('pointerup', end);
    c.addEventListener('pointercancel', end);
    c.addEventListener('wheel', (e) => {
      if (!e.ctrlKey && Math.abs(e.deltaY) < Math.abs(e.deltaX)) return;
      e.preventDefault();
      this.zoom(e.deltaY > 0 ? 1.15 : 1 / 1.15);
    }, { passive: false });
  }

  render(time) {
    if (time !== undefined && !this.dragging) this.time = time;
    const t0 = time !== undefined ? time : this.time;
    const { ctx } = this;
    const { w, h, dpr } = this.s;
    ctx.clearRect(0, 0, w, h);
    const cy = h / 2;
    const halfH = h / 2 - 2 * dpr;

    if (this.data) {
      const { binsPerSec, low, mid, high } = this.data;
      const secPerPx = this.windowSec / w;
      const start = t0 - this.windowSec / 2;
      const aL = new Float32Array(w), aM = new Float32Array(w), aH = new Float32Array(w);
      for (let x = 0; x < w; x++) {
        const ta = start + x * secPerPx;
        if (ta < 0 || ta > this.duration) continue;
        const ba = ta * binsPerSec, bb = (ta + secPerPx) * binsPerSec;
        aL[x] = bandMax(low, ba, bb) / 255;
        aM[x] = bandMax(mid, ba, bb) / 255;
        aH[x] = bandMax(high, ba, bb) / 255;
      }
      fillMirrored(ctx, aL, cy, halfH, COLORS.low);
      fillMirrored(ctx, aM, cy, halfH * 0.92, COLORS.mid);
      fillMirrored(ctx, aH, cy, halfH * 0.85, COLORS.high);

      // Beat grid
      const { bpm, beatOffset, downbeat } = this.data;
      if (bpm && beatOffset != null) {
        const period = 60 / bpm;
        const pxPerBeat = period / secPerPx;
        if (pxPerBeat > 6 * dpr) {
          const first = Math.ceil((start - beatOffset) / period);
          for (let k = first; ; k++) {
            const bt = beatOffset + k * period;
            const x = (bt - start) / secPerPx;
            if (x > w) break;
            if (bt < 0 || bt > this.duration) continue;
            const isBar = (((k - (downbeat || 0)) % 4) + 4) % 4 === 0;
            ctx.fillStyle = isBar ? 'rgba(255,255,255,0.55)' : 'rgba(255,255,255,0.18)';
            ctx.fillRect(Math.round(x), 0, Math.max(1, dpr), h);
            if (isBar) {
              ctx.fillStyle = 'rgba(255,255,255,0.9)';
              ctx.beginPath();
              ctx.moveTo(x - 4 * dpr, 0); ctx.lineTo(x + 4 * dpr, 0); ctx.lineTo(x, 5 * dpr);
              ctx.fill();
            }
          }
        }
      }
    } else {
      ctx.fillStyle = 'rgba(255,255,255,0.15)';
      ctx.fillRect(0, cy - dpr / 2, w, dpr);
    }

    // Centre playhead
    ctx.fillStyle = '#ff3b4e';
    ctx.fillRect(Math.round(w / 2 - dpr), 0, Math.max(2, 2 * dpr), h);
  }
}

// Bottom-anchored filled outline (Now Playing style).
function fillFromBottom(ctx, amps, h, color) {
  const n = amps.length;
  ctx.beginPath();
  ctx.moveTo(0, h);
  for (let x = 0; x < n; x++) ctx.lineTo(x, h - amps[x] * h);
  ctx.lineTo(n, h);
  ctx.closePath();
  ctx.fillStyle = color;
  ctx.fill();
}

export class OverviewWaveform {
  /**
   * style 'bands': 3-band colours, played part bright.
   * style 'mono':  single-colour mountain (played/unplayed colours), like a music-app scrubber.
   */
  constructor(canvas, { onSeek, style = 'bands', colors = {} } = {}) {
    this.style = style;
    this.colors = { played: '#6b3fe4', unplayed: 'rgba(107,63,228,0.28)', playhead: '#ff3b4e', empty: 'rgba(255,255,255,0.18)', emptyFill: 'rgba(255,255,255,0.8)', ...colors };
    this.canvas = canvas;
    this.data = null;
    this.duration = 0;
    this.progress = 0;
    this.onSeek = onSeek;
    this.bright = document.createElement('canvas');
    this.dim = document.createElement('canvas');
    const { ctx, state } = setupCanvas(canvas, () => { this.prerender(); this.render(); });
    this.ctx = ctx; this.s = state;
    this.bindPointer();
  }

  setData(analysis, duration) {
    this.data = analysis && analysis.low ? analysis : null;
    this.duration = duration || (analysis && analysis.duration) || 0;
    this.prerender();
    this.render();
  }

  prerender() {
    const { w, h } = this.s;
    for (const c of [this.bright, this.dim]) { c.width = w; c.height = h; }
    if (!this.data || !w) return;
    const { binsPerSec, low, mid, high } = this.data;
    const total = low.length;
    const bucket = total / w;
    const aL = new Float32Array(w), aM = new Float32Array(w), aH = new Float32Array(w);
    for (let x = 0; x < w; x++) {
      const a = x * bucket, b = (x + 1) * bucket;
      aL[x] = bandMax(low, a, b) / 255;
      aM[x] = bandMax(mid, a, b) / 255;
      aH[x] = bandMax(high, a, b) / 255;
    }
    void binsPerSec;
    if (this.style === 'mono') {
      const amps = new Float32Array(w);
      // light smoothing so it reads as a soft mountain line
      for (let x = 0; x < w; x++) amps[x] = Math.max(aL[x] * 0.9, aM[x], aH[x] * 0.8);
      const sm = new Float32Array(w);
      const r = Math.max(1, Math.round(w / 220));
      for (let x = 0; x < w; x++) {
        let acc = 0, n = 0;
        for (let k = -r; k <= r; k++) { const i = x + k; if (i >= 0 && i < w) { acc += amps[i]; n++; } }
        sm[x] = 0.12 + 0.88 * (acc / n);
      }
      fillFromBottom(this.bright.getContext('2d'), sm, h, this.colors.played);
      fillFromBottom(this.dim.getContext('2d'), sm, h, this.colors.unplayed);
      return;
    }
    const cy = h / 2, halfH = h / 2;
    for (const [c, alpha] of [[this.bright, 1], [this.dim, 0.32]]) {
      const g = c.getContext('2d');
      g.globalAlpha = alpha;
      fillMirrored(g, aL, cy, halfH, COLORS.low);
      fillMirrored(g, aM, cy, halfH * 0.92, COLORS.mid);
      fillMirrored(g, aH, cy, halfH * 0.85, COLORS.high);
    }
  }

  bindPointer() {
    const c = this.canvas;
    let active = false;
    const ratioOf = (e) => {
      const r = c.getBoundingClientRect();
      return Math.min(1, Math.max(0, (e.clientX - r.left) / r.width));
    };
    c.addEventListener('pointerdown', (e) => {
      if (!this.duration) return;
      active = true;
      c.setPointerCapture(e.pointerId);
      this.render(ratioOf(e));
    });
    c.addEventListener('pointermove', (e) => { if (active) this.render(ratioOf(e)); });
    const end = (e) => {
      if (!active) return;
      active = false;
      const r = ratioOf(e);
      this.onSeek && this.onSeek(r * this.duration);
    };
    c.addEventListener('pointerup', end);
    c.addEventListener('pointercancel', () => { active = false; });
  }

  render(progress) {
    if (progress !== undefined) this.progress = progress;
    const { ctx } = this;
    const { w, h, dpr } = this.s;
    ctx.clearRect(0, 0, w, h);
    const px = Math.round(this.progress * w);
    if (this.data) {
      ctx.drawImage(this.dim, 0, 0);
      if (px > 0) ctx.drawImage(this.bright, 0, 0, px, h, 0, 0, px, h);
    } else {
      const y = this.style === 'mono' ? h - 2 * dpr : h / 2 - dpr;
      ctx.fillStyle = this.colors.empty;
      ctx.fillRect(0, y, w, 2 * dpr);
      ctx.fillStyle = this.colors.emptyFill;
      ctx.fillRect(0, y, px, 2 * dpr);
    }
    ctx.fillStyle = this.colors.playhead;
    ctx.fillRect(Math.max(0, px - dpr), 0, Math.max(2, 2 * dpr), h);
  }
}
