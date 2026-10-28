// Bridge to the Android app shell (window.GrooveNative is injected by the app; absent in browsers).
const N = typeof window !== 'undefined' ? window.GrooveNative : null;

export const isApp = !!N;

/** Send playback state to Android (notification, lock screen, headphone buttons). */
export function nativeUpdate(info) {
  if (!N) return;
  try { N.update(JSON.stringify(info)); } catch (e) { console.warn('native update failed', e); }
}

/** Status/navigation bar colour to match the screen. */
export function nativeTheme(color, lightBars) {
  if (!N || !N.theme) return;
  try { N.theme(color, !!lightBars); } catch { /* older shell */ }
}

/** Commands from the notification, lock screen, headphones or audio focus changes. */
export function onRemote(handler) {
  window.grooveRemote = (cmd) => { try { handler(String(cmd)); } catch (e) { console.warn(e); } };
}

/** Android back button: return true if the app handled it (closed a sheet, went back a page). */
export function onBack(handler) {
  window.grooveBack = () => { try { return !!handler(); } catch { return false; } };
}

// ---------------------------------------------------------------- picking songs (app only)
// Results come back asynchronously through window.grooveNativeResult(json).
const waiting = { files: null, folder: null, rescan: null };
if (N) {
  window.grooveNativeResult = (json) => {
    let r;
    try { r = JSON.parse(json); } catch { return; }
    const w = waiting[r.type];
    waiting[r.type] = null;
    if (w) w(r);
  };
}
function ask(type, call) {
  return new Promise((resolve) => {
    if (waiting[type]) waiting[type]({ type, cancelled: true });
    waiting[type] = resolve;
    try { call(); } catch (e) { waiting[type] = null; resolve({ type, error: String(e) }); }
  });
}
/** → { items: [{uri, name, size, mime, path}], grants, grantLimit } or { cancelled } / { error } */
export const pickFiles = () => ask('files', () => N.pickFiles());
/** → { folder: {uri, name}, items: [...] } */
export const pickFolder = () => ask('folder', () => N.pickFolder());
/** → { folders: [{uri, ok, items}] } */
export const rescanFolders = (uris) => ask('rescan', () => N.rescanFolders(JSON.stringify(uris)));
export function releaseUri(uri) { try { N && N.release(uri); } catch { /* ignore */ } }

/** URL the WebView can stream (with seeking) for a song on the phone. */
export function mediaUrl(uri) { return `/media?u=${encodeURIComponent(uri)}`; }

/**
 * A song on the phone, readable like a File: .size, .name, .type, .slice(a, z).arrayBuffer().
 * Reads use HTTP Range requests, so reading tags only touches the start and end of the file.
 */
export class RemoteFile {
  constructor(uri, name, size, type) {
    this.uri = uri;
    this.url = mediaUrl(uri);
    this.name = name || '';
    this.size = size;
    this.type = type || '';
    this.chunks = []; // cached {start, bytes}
  }
  /** Needed when the picker didn't report a size. */
  async ensureSize() {
    if (this.size > 0) return this.size;
    const r = await fetch(this.url, { headers: { Range: 'bytes=0-0' } });
    const m = /\/(\d+)\s*$/.exec(r.headers.get('Content-Range') || '');
    this.size = m ? +m[1] : +(r.headers.get('Content-Length') || 0);
    await r.arrayBuffer().catch(() => {});
    return this.size;
  }
  async read(start, end) { // [start, end)
    for (const c of this.chunks) {
      if (start >= c.start && end <= c.start + c.bytes.length) return c.bytes.slice(start - c.start, end - c.start);
    }
    // Read a little ahead: tag parsers make many small reads near each other.
    const want = Math.max(end, Math.min(this.size, start + 256 * 1024));
    const r = await fetch(this.url, { headers: { Range: `bytes=${start}-${want - 1}` } });
    if (!r.ok) throw new Error(`read failed (${r.status})`);
    let bytes = new Uint8Array(await r.arrayBuffer());
    if (r.status === 200 && start > 0) bytes = bytes.slice(start, want); // server ignored the range
    if (this.chunks.length > 6) this.chunks.shift();
    this.chunks.push({ start, bytes });
    return bytes.slice(0, end - start);
  }
  slice(a = 0, z = this.size) {
    const start = Math.max(0, Math.min(a, this.size)), end = Math.max(start, Math.min(z, this.size));
    const file = this;
    return {
      size: end - start,
      async arrayBuffer() { const b = await file.read(start, end); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); },
    };
  }
  async arrayBuffer() {
    const r = await fetch(this.url);
    if (!r.ok) throw new Error(`read failed (${r.status})`);
    return r.arrayBuffer();
  }
}

export function blobToBase64(blob) {
  return new Promise((resolve) => {
    const r = new FileReader();
    r.onload = () => resolve(String(r.result).split(',')[1] || '');
    r.onerror = () => resolve('');
    r.readAsDataURL(blob);
  });
}
