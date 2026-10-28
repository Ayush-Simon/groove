// Decodes a track at a reduced rate (for analysis only — playback always uses the
// original file) and hands the samples to the worker.

const ANALYSIS_RATE = 22050;
let worker = null;
let seq = 0;
const pending = new Map();

// The worker is created from a Blob of its own source, which works the same in browsers
// and inside the Android app's WebView (where worker scripts come from app assets).
let workerUrl = null;
async function getWorker() {
  if (!worker) {
    if (!workerUrl) {
      const src = await (await fetch('js/analyzer-worker.js')).text();
      workerUrl = URL.createObjectURL(new Blob([src], { type: 'text/javascript' }));
    }
    worker = new Worker(workerUrl);
    worker.onmessage = (e) => {
      const { id, result, error } = e.data;
      const p = pending.get(id);
      if (!p) return;
      pending.delete(id);
      if (error) p.reject(new Error(error));
      else p.resolve(result);
    };
    worker.onerror = (e) => {
      for (const p of pending.values()) p.reject(new Error(e.message || 'Analysis worker failed'));
      pending.clear();
      worker = null;
    };
  }
  return worker;
}

function decode(arrayBuffer, rate = ANALYSIS_RATE) {
  const OAC = window.OfflineAudioContext || window.webkitOfflineAudioContext;
  const ctx = new OAC(1, 1, rate);
  return new Promise((resolve, reject) => {
    // Older Safari only supports the callback form.
    const p = ctx.decodeAudioData(arrayBuffer, resolve, reject);
    if (p && p.then) p.then(resolve, reject);
  });
}

export async function analyzeBlob(blob) {
  const ab = await blob.arrayBuffer();
  const buffer = await decode(ab);
  const n = buffer.length;
  const mono = new Float32Array(n);
  const chs = buffer.numberOfChannels;
  for (let c = 0; c < chs; c++) {
    const d = buffer.getChannelData(c);
    for (let i = 0; i < n; i++) mono[i] += d[i] / chs;
  }
  const id = ++seq;
  const w = await getWorker();
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    w.postMessage({ id, samples: mono, sampleRate: buffer.sampleRate }, [mono.buffer]);
  });
}

/** Full-quality decode (used when the phone's player can't open a file directly). */
export function decodeFull(arrayBuffer, rate) { return decode(arrayBuffer, rate); }
