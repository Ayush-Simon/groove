/* Track analysis worker: 3-band DJ waveform, BPM + beat grid, musical key.
 * Input:  { id, samples: Float32Array (mono), sampleRate }
 * Output: { id, result } or { id, error }
 * Runs off the main thread so the UI never freezes.
 */

const BINS_PER_SEC_TARGET = 100;

function biquad(type, f0, sr, q = Math.SQRT1_2) {
  const w0 = (2 * Math.PI * f0) / sr;
  const cos = Math.cos(w0);
  const alpha = Math.sin(w0) / (2 * q);
  const a0 = 1 + alpha;
  let b0, b1, b2;
  if (type === 'lp') { b0 = (1 - cos) / 2; b1 = 1 - cos; b2 = (1 - cos) / 2; }
  else { b0 = (1 + cos) / 2; b1 = -(1 + cos); b2 = (1 + cos) / 2; }
  return { b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: (-2 * cos) / a0, a2: (1 - alpha) / a0, z1: 0, z2: 0 };
}

// Transposed direct form II, one sample.
function step(f, x) {
  const y = f.b0 * x + f.z1;
  f.z1 = f.b1 * x - f.a1 * y + f.z2;
  f.z2 = f.b2 * x - f.a2 * y;
  return y;
}

function bandSplit(samples, sr) {
  const binSize = Math.max(1, Math.round(sr / BINS_PER_SEC_TARGET));
  const nBins = Math.ceil(samples.length / binSize);
  const pk = [new Float32Array(nBins), new Float32Array(nBins), new Float32Array(nBins)];
  const en = [new Float32Array(nBins), new Float32Array(nBins), new Float32Array(nBins)];

  // Rekordbox-style bands: low < 250 Hz, mid 250 Hz–4 kHz, high > 4 kHz (4th-order filters)
  const lo1 = biquad('lp', 250, sr), lo2 = biquad('lp', 250, sr);
  const mh1 = biquad('hp', 250, sr), mh2 = biquad('hp', 250, sr);
  const ml1 = biquad('lp', 4000, sr), ml2 = biquad('lp', 4000, sr);
  const hi1 = biquad('hp', 4000, sr), hi2 = biquad('hp', 4000, sr);

  for (let b = 0, i = 0; b < nBins; b++) {
    const end = Math.min(samples.length, i + binSize);
    let pl = 0, pm = 0, ph = 0, el = 0, em = 0, eh = 0;
    for (; i < end; i++) {
      const x = samples[i];
      const l = step(lo2, step(lo1, x));
      const m = step(ml2, step(ml1, step(mh2, step(mh1, x))));
      const h = step(hi2, step(hi1, x));
      const al = l < 0 ? -l : l, am = m < 0 ? -m : m, ah = h < 0 ? -h : h;
      if (al > pl) pl = al;
      if (am > pm) pm = am;
      if (ah > ph) ph = ah;
      el += l * l; em += m * m; eh += h * h;
    }
    pk[0][b] = pl; pk[1][b] = pm; pk[2][b] = ph;
    en[0][b] = el / binSize; en[1][b] = em / binSize; en[2][b] = eh / binSize;
  }
  return { binSize, fps: sr / binSize, nBins, pk, en };
}

function percentile(arr, p) {
  const step = Math.max(1, Math.floor(arr.length / 20000));
  const s = [];
  for (let i = 0; i < arr.length; i += step) s.push(arr[i]);
  s.sort((a, b) => a - b);
  return s[Math.min(s.length - 1, Math.floor(p * s.length))] || 0;
}

function waveformBytes(pk) {
  const n = pk[0].length;
  const maxes = new Float32Array(n);
  for (let i = 0; i < n; i++) maxes[i] = Math.max(pk[0][i], pk[1][i], pk[2][i]);
  const ref = percentile(maxes, 0.995) || 1;
  return pk.map((band) => {
    const out = new Uint8Array(n);
    for (let i = 0; i < n; i++) out[i] = Math.round(255 * Math.pow(Math.min(1, band[i] / ref), 0.8));
    return out;
  });
}

// ---------------- Tempo ----------------
function onsetEnvelope(en) {
  const n = en[0].length;
  const weights = [1.0, 1.0, 0.7];
  const onset = new Float32Array(n);
  const lowOnset = new Float32Array(n);
  for (let b = 0; b < 3; b++) {
    let prev = 0;
    for (let i = 0; i < n; i++) {
      const v = Math.log1p(1000 * Math.sqrt(en[b][i]));
      const d = v - prev;
      prev = v;
      if (d > 0) {
        onset[i] += weights[b] * d;
        if (b === 0) lowOnset[i] = d;
      }
    }
  }
  // Remove slow trends: subtract a ~1 s moving average, keep the positive part.
  const win = 100;
  const out = new Float32Array(n);
  let acc = 0;
  for (let i = 0; i < n; i++) {
    acc += onset[i];
    if (i >= win) acc -= onset[i - win];
    const mean = acc / Math.min(i + 1, win);
    out[i] = Math.max(0, onset[i] - mean);
  }
  return { onset: smooth(out, 1.5), lowOnset: smooth(lowOnset, 1.5), raw: onset };
}

// Gaussian smoothing so beat periods that fall between two frames still produce a clear peak.
function smooth(arr, sigma) {
  const r = Math.ceil(sigma * 3);
  const k = [];
  let sum = 0;
  for (let i = -r; i <= r; i++) { const v = Math.exp(-(i * i) / (2 * sigma * sigma)); k.push(v); sum += v; }
  const out = new Float32Array(arr.length);
  for (let i = 0; i < arr.length; i++) {
    let acc = 0;
    for (let j = -r; j <= r; j++) {
      const x = i + j;
      if (x >= 0 && x < arr.length) acc += arr[x] * k[j + r];
    }
    out[i] = acc / sum;
  }
  return out;
}

function sampleAt(arr, x) {
  const i = Math.floor(x);
  if (i < 0 || i + 1 >= arr.length) return 0;
  const f = x - i;
  return arr[i] * (1 - f) + arr[i + 1] * f;
}

function combScore(onset, period, phase) {
  let s = 0, c = 0;
  for (let x = phase; x < onset.length - 1; x += period) { s += sampleAt(onset, x); c++; }
  return c ? s / c : 0;
}

function bestPhase(onset, period) {
  let best = -1, bestPh = 0;
  for (let ph = 0; ph < period; ph += 0.5) {
    const s = combScore(onset, period, ph);
    if (s > best) { best = s; bestPh = ph; }
  }
  return { score: best, phase: bestPh };
}

function detectTempo(onset, lowOnset, raw, fps) {
  const n = onset.length;
  if (n < fps * 8) return null; // under ~8 s: not enough to judge
  // Steady tones (test signals, drones, ambient) have almost no onsets at all.
  let rawMean = 0;
  for (let i = 0; i < n; i++) rawMean += raw[i];
  rawMean /= n;
  if (rawMean < 0.01) return null;
  // Beats are placed using an envelope that favours the kick drum, so the grid
  // lands on the beat rather than on off-beat hi-hats.
  const phaseEnv = new Float32Array(n);
  for (let i = 0; i < n; i++) phaseEnv[i] = onset[i] + 2 * lowOnset[i];

  // Autocorrelation over 55–200 BPM.
  const lagMin = Math.floor((60 * fps) / 200);
  const lagMax = Math.ceil((60 * fps) / 55);
  const acf = new Float32Array(lagMax * 2 + 2);
  for (let L = 1; L < acf.length; L++) {
    let s = 0;
    for (let i = 0; i + L < n; i++) s += onset[i] * onset[i + L];
    acf[L] = s / (n - L);
  }
  let mean = 0;
  for (let L = lagMin; L <= lagMax; L++) mean += acf[L];
  mean /= lagMax - lagMin + 1;

  let bestL = -1, bestScore = -Infinity;
  for (let L = lagMin; L <= lagMax; L++) {
    const bpm = (60 * fps) / L;
    const prior = Math.exp(-0.5 * Math.pow(Math.log2(bpm / 122) / 0.85, 2));
    const score = prior * (acf[L] + 0.5 * acf[2 * L]);
    if (score > bestScore) { bestScore = score; bestL = L; }
  }
  if (bestL < 0 || mean <= 0) return null;
  const peakiness = acf[bestL] / mean;

  // Parabolic refinement of the lag.
  const y0 = acf[bestL - 1], y1 = acf[bestL], y2 = acf[bestL + 1];
  const denom = y0 - 2 * y1 + y2;
  const lagF = bestL + (denom !== 0 ? (0.5 * (y0 - y2)) / denom : 0);
  let bpm = (60 * fps) / lagF;
  while (bpm < 70) bpm *= 2;
  while (bpm > 180) bpm /= 2;

  // Fine search: tempo ±1.5 BPM in 0.02 steps, scored by how well a fixed grid
  // lines up with onsets across the whole track. This also gives the beat phase.
  // Autocorrelation often lands on a related tempo (half, double, 2/3 …). Re-check those
  // candidates by how well a grid at that tempo lines up with kicks across the track.
  const gentlePrior = (b) => Math.exp(-0.5 * Math.pow(Math.log2(b / 122) / 1.3, 2));
  let pick = { bpm, score: bestPhase(phaseEnv, (60 * fps) / bpm).score * gentlePrior(bpm) };
  for (const ratio of [2, 0.5, 1.5, 2 / 3, 4 / 3, 0.75]) {
    const b = bpm * ratio;
    if (b < 70 || b > 180) continue;
    const sc = bestPhase(phaseEnv, (60 * fps) / b).score * gentlePrior(b);
    if (sc > pick.score * 1.05) pick = { bpm: b, score: sc };
  }
  bpm = pick.bpm;

  let best = { score: -1, bpm, phase: 0 };
  for (let b = bpm - 1.5; b <= bpm + 1.5; b += 0.02) {
    const r = bestPhase(phaseEnv, (60 * fps) / b);
    if (r.score > best.score) best = { score: r.score, bpm: b, phase: r.phase };
  }
  // Prefer a whole-number BPM when it fits (almost) as well — most dance music is.
  const whole = Math.round(best.bpm);
  const rw = bestPhase(phaseEnv, (60 * fps) / whole);
  if (rw.score >= best.score * 0.97) best = { score: rw.score, bpm: whole, phase: rw.phase };

  // Confidence: grid alignment vs. a random grid.
  const avg = phaseEnv.reduce((a, v) => a + v, 0) / n;
  const gridContrast = avg > 0 ? best.score / avg : 0;

  // Downbeat: which of the 4 beat positions carries the most kick energy.
  const period = (60 * fps) / best.bpm;
  const bins = [0, 0, 0, 0];
  for (let k = 0, x = best.phase; x < n - 1; k++, x += period) bins[k % 4] += sampleAt(lowOnset, x);
  const downbeat = bins.indexOf(Math.max(...bins));

  const confident = peakiness > 1.35 && gridContrast > 1.6;
  return {
    bpm: Math.round(best.bpm * 100) / 100,
    beatOffset: best.phase / fps,
    downbeat,
    confidence: Math.min(1, (gridContrast - 1) / 3),
    confident,
  };
}

// ---------------- Key ----------------
function makeFFT(N) {
  const levels = Math.log2(N);
  const rev = new Uint32Array(N);
  for (let i = 0; i < N; i++) {
    let r = 0;
    for (let b = 0; b < levels; b++) r |= ((i >> b) & 1) << (levels - 1 - b);
    rev[i] = r;
  }
  const cos = new Float64Array(N / 2), sin = new Float64Array(N / 2);
  for (let i = 0; i < N / 2; i++) { cos[i] = Math.cos((2 * Math.PI * i) / N); sin[i] = -Math.sin((2 * Math.PI * i) / N); }
  const re = new Float64Array(N), im = new Float64Array(N);
  return function fft(input) {
    for (let i = 0; i < N; i++) { re[rev[i]] = input[i]; im[rev[i]] = 0; }
    for (let size = 2; size <= N; size <<= 1) {
      const half = size >> 1, stepT = N / size;
      for (let i = 0; i < N; i += size) {
        for (let j = 0, k = 0; j < half; j++, k += stepT) {
          const a = i + j, b = a + half;
          const tr = re[b] * cos[k] - im[b] * sin[k];
          const ti = re[b] * sin[k] + im[b] * cos[k];
          re[b] = re[a] - tr; im[b] = im[a] - ti;
          re[a] += tr; im[a] += ti;
        }
      }
    }
    return { re, im };
  };
}

// Key profiles from KeyFinder (Ibrahim Sha'ath), tuned for real recordings.
const MAJOR = [7.24, 3.50, 3.58, 2.85, 5.82, 4.56, 2.45, 6.99, 3.39, 4.56, 4.07, 4.46];
const MINOR = [7.00, 3.14, 4.36, 5.40, 3.67, 4.09, 3.91, 6.20, 3.63, 2.87, 5.35, 3.83];
const MAJOR_NAMES = ['C', 'D♭', 'D', 'E♭', 'E', 'F', 'F♯', 'G', 'A♭', 'A', 'B♭', 'B'];
const MINOR_NAMES = ['C', 'C♯', 'D', 'E♭', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'B♭', 'B'];
const CAMELOT_MAJOR = ['8B', '3B', '10B', '5B', '12B', '7B', '2B', '9B', '4B', '11B', '6B', '1B'];
const CAMELOT_MINOR = ['5A', '12A', '7A', '2A', '9A', '4A', '11A', '6A', '1A', '8A', '3A', '10A'];

function pearson(x, y) {
  const n = x.length;
  let sx = 0, sy = 0;
  for (let i = 0; i < n; i++) { sx += x[i]; sy += y[i]; }
  const mx = sx / n, my = sy / n;
  let num = 0, dx = 0, dy = 0;
  for (let i = 0; i < n; i++) {
    const a = x[i] - mx, b = y[i] - my;
    num += a * b; dx += a * a; dy += b * b;
  }
  return dx && dy ? num / Math.sqrt(dx * dy) : 0;
}

function detectKey(samples, sr) {
  const N = 8192;
  if (samples.length < N * 4) return null;
  const fft = makeFFT(N);
  const win = new Float64Array(N);
  for (let i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (N - 1));

  // Map FFT bins (55 Hz – 1.76 kHz, A1–A6) to pitch classes.
  const kMin = Math.ceil((55 * N) / sr), kMax = Math.floor((1760 * N) / sr);
  const pcOf = new Int8Array(kMax + 1).fill(-1);
  for (let k = kMin; k <= kMax; k++) {
    const midi = 69 + 12 * Math.log2(((k * sr) / N) / 440);
    const nearest = Math.round(midi);
    if (Math.abs(midi - nearest) <= 0.42) pcOf[k] = ((nearest % 12) + 12) % 12;
  }

  const chroma = new Float64Array(12);
  const frame = new Float64Array(N);
  const mags = new Float64Array(kMax + 2);
  const hop = N;
  const maxFrames = 1200;
  const total = Math.floor((samples.length - N) / hop);
  const stride = Math.max(1, Math.floor(total / maxFrames));
  let used = 0;
  for (let f = 0; f < total; f += stride) {
    const off = f * hop;
    let energy = 0;
    for (let i = 0; i < N; i++) { const v = samples[off + i]; frame[i] = v * win[i]; energy += v * v; }
    if (energy / N < 1e-7) continue; // skip silence
    const { re, im } = fft(frame);
    for (let k = kMin - 1; k <= kMax + 1; k++) mags[k] = Math.hypot(re[k], im[k]);
    const c = new Float64Array(12);
    for (let k = kMin; k <= kMax; k++) {
      // spectral peaks only: reduces smeared noise
      if (pcOf[k] < 0 || mags[k] < mags[k - 1] || mags[k] < mags[k + 1]) continue;
      c[pcOf[k]] += mags[k];
    }
    let mx = 0;
    for (let i = 0; i < 12; i++) if (c[i] > mx) mx = c[i];
    if (mx <= 0) continue;
    for (let i = 0; i < 12; i++) chroma[i] += c[i] / mx;
    used++;
  }
  if (used < 4) return null;

  const scores = [];
  for (let t = 0; t < 12; t++) {
    const rot = new Float64Array(12);
    for (let i = 0; i < 12; i++) rot[i] = chroma[(i + t) % 12];
    scores.push({ t, mode: 'major', r: pearson(rot, MAJOR) });
    scores.push({ t, mode: 'minor', r: pearson(rot, MINOR) });
  }
  scores.sort((a, b) => b.r - a.r);
  const best = scores[0];
  const margin = best.r - scores[1].r;
  const isMajor = best.mode === 'major';
  return {
    key: `${isMajor ? MAJOR_NAMES[best.t] : MINOR_NAMES[best.t]} ${best.mode}`,
    short: isMajor ? MAJOR_NAMES[best.t] : `${MINOR_NAMES[best.t]}m`,
    camelot: isMajor ? CAMELOT_MAJOR[best.t] : CAMELOT_MINOR[best.t],
    strength: best.r,
    confident: best.r > 0.55,
    margin,
  };
}

self.onmessage = (e) => {
  const { id, samples, sampleRate } = e.data;
  try {
    const split = bandSplit(samples, sampleRate);
    const [low, mid, high] = waveformBytes(split.pk);
    const { onset, lowOnset, raw } = onsetEnvelope(split.en);
    const tempo = detectTempo(onset, lowOnset, raw, split.fps);
    const key = detectKey(samples, sampleRate);
    const result = {
      version: 1,
      duration: samples.length / sampleRate,
      binsPerSec: split.fps,
      low, mid, high,
      bpm: tempo && tempo.confident ? tempo.bpm : null,
      beatOffset: tempo && tempo.confident ? tempo.beatOffset : null,
      downbeat: tempo && tempo.confident ? tempo.downbeat : null,
      bpmConfidence: tempo ? tempo.confidence : 0,
      key: key && key.confident ? key.key : null,
      keyShort: key && key.confident ? key.short : null,
      camelot: key && key.confident ? key.camelot : null,
      keyStrength: key ? key.strength : 0,
    };
    self.postMessage({ id, result }, [low.buffer, mid.buffer, high.buffer]);
  } catch (err) {
    self.postMessage({ id, error: String(err && err.message || err) });
  }
};
