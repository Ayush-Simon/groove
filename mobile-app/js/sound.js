// Sound effects chain: 10-band equaliser + sound stage (3D Spatial / Headphones / Speakers).
// input → EQ (10 biquads) → headroom gain → stage → output
// With the EQ flat and the stage off, the signal passes through unchanged.

export const EQ_FREQS = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];
export const EQ_LABELS = ['31', '62', '125', '250', '500', '1k', '2k', '4k', '8k', '16k'];

export const PRESETS = {
  flat:        { name: 'Flat',           gains: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0] },
  bass:        { name: 'Bass Boost',     gains: [6, 5, 4, 2, 0, 0, 0, 0, 0, 0] },
  bassCut:     { name: 'Bass Reducer',   gains: [-6, -5, -4, -2, 0, 0, 0, 0, 0, 0] },
  treble:      { name: 'Treble Boost',   gains: [0, 0, 0, 0, 0, 1, 2, 4, 5, 6] },
  vocal:       { name: 'Vocal',          gains: [-2, -2, -1, 0, 2, 4, 4, 3, 1, 0] },
  acoustic:    { name: 'Acoustic',       gains: [3, 3, 2, 1, 1, 1, 2, 3, 2, 1] },
  electronic:  { name: 'Electronic',     gains: [5, 4, 1, 0, -2, 1, 0, 1, 4, 5] },
  hiphop:      { name: 'Hip-Hop',        gains: [5, 4, 2, 3, -1, -1, 1, -1, 2, 3] },
  rock:        { name: 'Rock',           gains: [4, 3, 2, 1, -1, -1, 0, 2, 3, 4] },
  pop:         { name: 'Pop',            gains: [-1, 1, 3, 4, 3, 0, -1, -1, 0, 1] },
  jazz:        { name: 'Jazz',           gains: [3, 2, 1, 2, -1, -1, 0, 1, 2, 3] },
  classical:   { name: 'Classical',      gains: [4, 3, 2, 1, -1, -1, 0, 2, 3, 4] },
  lateNight:   { name: 'Late Night',     gains: [4, 3, 1, 0, -1, 0, 0, 1, 2, 3] },
  smallSpk:    { name: 'Small Speakers', gains: [5, 4, 3, 2, 1, 0, 0, -1, -1, -2] },
};

export const STAGES = {
  off:        { name: 'Off',        desc: 'Plain stereo, exactly as mixed.' },
  spatial:    { name: '3D Spatial', desc: 'Places the music around you, with a sense of room behind and above. Best on headphones.' },
  headphones: { name: 'Headphones', desc: 'Moves the sound out of your head to two virtual speakers in front of you, like listening in a studio.' },
  speakers:   { name: 'Speakers',   desc: 'Widens the stereo image on laptop and phone speakers or a pair of desktop speakers.' },
};

function impulse(ctx, seconds, decay, preDelay = 0.01) {
  const rate = ctx.sampleRate;
  const len = Math.floor(rate * seconds);
  const buf = ctx.createBuffer(2, len, rate);
  const pre = Math.floor(rate * preDelay);
  for (let c = 0; c < 2; c++) {
    const d = buf.getChannelData(c);
    for (let i = pre; i < len; i++) d[i] = (Math.random() * 2 - 1) * Math.pow(1 - (i - pre) / (len - pre), decay);
  }
  return buf;
}

function panner(ctx, azimuthDeg, elevation = 0) {
  const p = ctx.createPanner();
  p.panningModel = 'HRTF';
  p.distanceModel = 'inverse';
  p.refDistance = 1;
  p.rolloffFactor = 0;
  const a = (azimuthDeg * Math.PI) / 180;
  const x = Math.sin(a), z = -Math.cos(a);
  if (p.positionX) { p.positionX.value = x; p.positionY.value = elevation; p.positionZ.value = z; }
  else p.setPosition(x, elevation, z);
  return p;
}

export class SoundFX {
  constructor(ctx) {
    this.ctx = ctx;
    this.input = ctx.createGain();
    this.output = ctx.createGain();
    this.eqOn = false;
    this.gains = PRESETS.flat.gains.slice();
    this.stage = 'off';
    this.intensity = 0.6;

    // EQ
    this.bands = EQ_FREQS.map((f, i) => {
      const b = ctx.createBiquadFilter();
      b.type = i === 0 ? 'lowshelf' : i === EQ_FREQS.length - 1 ? 'highshelf' : 'peaking';
      b.frequency.value = f;
      b.Q.value = 1.1;
      b.gain.value = 0;
      return b;
    });
    this.headroom = ctx.createGain();
    this.input.connect(this.bands[0]);
    for (let i = 0; i < this.bands.length - 1; i++) this.bands[i].connect(this.bands[i + 1]);
    this.bands[this.bands.length - 1].connect(this.headroom);

    // Stage: dry path + one wet path per mode
    this.dry = ctx.createGain();
    this.headroom.connect(this.dry).connect(this.output);
    this.paths = {
      spatial: this.buildSpatial(),
      headphones: this.buildHeadphones(),
      speakers: this.buildSpeakers(),
    };
    for (const p of Object.values(this.paths)) { p.wet.gain.value = 0; p.out.connect(p.wet).connect(this.output); }
    this.apply();
  }

  // Two virtual speakers at ±30° (HRTF) plus faint early reflections.
  buildHeadphones() {
    const ctx = this.ctx;
    const split = ctx.createChannelSplitter(2);
    const out = ctx.createGain();
    const l = panner(ctx, -30), r = panner(ctx, 30);
    split.connect(l, 0); split.connect(r, 1);
    const makeup = ctx.createGain(); makeup.gain.value = 1.25;
    l.connect(makeup); r.connect(makeup);
    makeup.connect(out);
    const room = ctx.createConvolver(); room.buffer = impulse(ctx, 0.35, 3.5);
    const roomGain = ctx.createGain(); roomGain.gain.value = 0.12;
    room.connect(roomGain).connect(out);
    return { inNode: split, extraIn: room, out, wet: ctx.createGain() };
  }

  // Wider virtual speakers at ±50°, a centre image, and a reverb tail placed behind and above.
  buildSpatial() {
    const ctx = this.ctx;
    const split = ctx.createChannelSplitter(2);
    const out = ctx.createGain();
    const l = panner(ctx, -50, 0.15), r = panner(ctx, 50, 0.15);
    split.connect(l, 0); split.connect(r, 1);
    const mid = ctx.createGain(); mid.gain.value = 0.5;
    split.connect(mid, 0); split.connect(mid, 1);
    const c = panner(ctx, 0, 0.1);
    mid.connect(c);
    const cGain = ctx.createGain(); cGain.gain.value = 0.55;
    c.connect(cGain).connect(out);
    const makeup = ctx.createGain(); makeup.gain.value = 1.1;
    l.connect(makeup); r.connect(makeup); makeup.connect(out);
    const verb = ctx.createConvolver(); verb.buffer = impulse(ctx, 1.8, 2.6, 0.025);
    const vs = ctx.createChannelSplitter(2);
    verb.connect(vs);
    const rl = panner(ctx, -130, 0.6), rr = panner(ctx, 130, 0.6);
    vs.connect(rl, 0); vs.connect(rr, 1);
    const verbGain = ctx.createGain(); verbGain.gain.value = 0.22;
    rl.connect(verbGain); rr.connect(verbGain); verbGain.connect(out);
    return { inNode: split, extraIn: verb, out, wet: ctx.createGain() };
  }

  // Mid/side widener: L' = L·a + R·b, R' = R·a + L·b with a = (1+w)/2, b = (1−w)/2.
  buildSpeakers() {
    const ctx = this.ctx;
    const split = ctx.createChannelSplitter(2);
    const merge = ctx.createChannelMerger(2);
    this.wLL = ctx.createGain(); this.wRL = ctx.createGain();
    this.wRR = ctx.createGain(); this.wLR = ctx.createGain();
    split.connect(this.wLL, 0); this.wLL.connect(merge, 0, 0);
    split.connect(this.wRL, 1); this.wRL.connect(merge, 0, 0);
    split.connect(this.wRR, 1); this.wRR.connect(merge, 0, 1);
    split.connect(this.wLR, 0); this.wLR.connect(merge, 0, 1);
    const out = ctx.createGain();
    merge.connect(out);
    return { inNode: split, extraIn: null, out, wet: ctx.createGain() };
  }

  setEq(on, gains) {
    this.eqOn = on;
    if (gains) this.gains = gains.slice();
    this.apply();
  }
  setStage(stage, intensity) {
    this.stage = STAGES[stage] ? stage : 'off';
    if (intensity != null) this.intensity = intensity;
    this.apply();
  }

  /** Peak boost of the current EQ curve, used to lower the level so boosts can't clip. */
  peakDb() {
    if (!this.eqOn) return 0;
    const n = 256;
    const freqs = new Float32Array(n), mag = new Float32Array(n), ph = new Float32Array(n), total = new Float32Array(n).fill(1);
    for (let i = 0; i < n; i++) freqs[i] = 20 * Math.pow(1000, i / (n - 1));
    for (const b of this.bands) {
      b.getFrequencyResponse(freqs, mag, ph);
      for (let i = 0; i < n; i++) total[i] *= mag[i];
    }
    let peak = 0;
    for (let i = 0; i < n; i++) peak = Math.max(peak, 20 * Math.log10(total[i]));
    return peak;
  }

  apply() {
    const t = this.ctx.currentTime;
    const ramp = (param, v) => param.setTargetAtTime(v, t, 0.03);
    this.bands.forEach((b, i) => { b.gain.value = this.eqOn ? this.gains[i] : 0; });
    ramp(this.headroom.gain, Math.pow(10, -this.peakDb() / 20));

    const k = this.intensity;
    for (const [name, p] of Object.entries(this.paths)) {
      const on = this.stage === name;
      if (on && !p.connected) {
        this.headroom.connect(p.inNode);
        if (p.extraIn) this.headroom.connect(p.extraIn);
        p.connected = true;
      }
      ramp(p.wet.gain, on ? (name === 'speakers' ? 1 : 0.35 + 0.65 * k) : 0);
      if (!on && p.connected) {
        // Disconnect after the fade-out so idle effects don't use CPU.
        setTimeout(() => {
          if (this.stage === name) return;
          try { this.headroom.disconnect(p.inNode); if (p.extraIn) this.headroom.disconnect(p.extraIn); } catch { /* already */ }
          p.connected = false;
        }, 200);
      }
    }
    const w = 1 + 1.2 * k; // speakers width
    const a = (1 + w) / 2, b = (1 - w) / 2, norm = 1 / Math.max(1, a);
    for (const [g, v] of [[this.wLL, a], [this.wRR, a], [this.wRL, b], [this.wLR, b]]) ramp(g.gain, v * norm);
    let dry = 1;
    if (this.stage === 'spatial' || this.stage === 'headphones') dry = 0.65 * (1 - k);
    if (this.stage === 'speakers') dry = 0;
    ramp(this.dry.gain, dry);
  }

  get active() { return (this.eqOn && this.gains.some((g) => g !== 0)) || this.stage !== 'off'; }
}
