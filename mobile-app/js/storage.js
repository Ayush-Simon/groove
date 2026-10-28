// Library store.
//
// Browser: session-only and in memory. Songs are references to your original files (File
// objects): nothing is copied or written to disk, and everything is forgotten when the tab closes.
//
// Android app: the library is saved, so it's still there next time. Only small things are
// stored — song details, key/BPM/waveform analysis, playlists and cover thumbnails. The music
// itself is never copied; each song keeps a link (content URI) to the file on your phone.

import { isApp } from './native.js';

export const persistent = isApp;

const STORES = ['tracks', 'files', 'art', 'thumbs', 'playlists', 'folders', 'meta'];
const keyPaths = { tracks: 'id', playlists: 'id', folders: 'uri' };

// ---------------------------------------------------------------- memory backend
const mem = Object.fromEntries(STORES.map((s) => [s, new Map()]));
const keyOf = (store, v, key) => (key !== undefined ? key : v[keyPaths[store]]);

const memory = {
  async getAll(store) { return [...mem[store].values()]; },
  async entries(store) { return [...mem[store].entries()]; },
  async get(store, key) { return mem[store].get(key); },
  async put(store, value, key) { mem[store].set(keyOf(store, value, key), value); },
  async del(store, key) { mem[store].delete(key); },
  async putMany(store, values) { for (const v of values) mem[store].set(keyOf(store, v), v); },
  async clear() { for (const s of STORES) mem[s].clear(); },
};

// ---------------------------------------------------------------- IndexedDB backend (app)
const DB_NAME = 'groove-library';
let dbp = null;
function openDb() {
  if (!dbp) {
    dbp = new Promise((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, 1);
      req.onupgradeneeded = () => {
        const db = req.result;
        for (const s of STORES) {
          if (s === 'files' || db.objectStoreNames.contains(s)) continue;
          db.createObjectStore(s, keyPaths[s] ? { keyPath: keyPaths[s] } : undefined);
        }
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }
  return dbp;
}
function tx(store, mode, fn) {
  return openDb().then((db) => new Promise((resolve, reject) => {
    const t = db.transaction(store, mode);
    const os = t.objectStore(store);
    let result;
    const r = fn(os);
    if (r) r.onsuccess = () => { result = r.result; };
    t.oncomplete = () => resolve(result);
    t.onerror = t.onabort = () => reject(t.error);
  }));
}
const idb = {
  getAll: (store) => tx(store, 'readonly', (os) => os.getAll()),
  async entries(store) {
    const [keys, values] = await Promise.all([tx(store, 'readonly', (os) => os.getAllKeys()), tx(store, 'readonly', (os) => os.getAll())]);
    return keys.map((k, i) => [k, values[i]]);
  },
  get: (store, key) => tx(store, 'readonly', (os) => os.get(key)),
  put: (store, value, key) => tx(store, 'readwrite', (os) => (keyPaths[store] ? os.put(value) : os.put(value, key))),
  del: (store, key) => tx(store, 'readwrite', (os) => os.delete(key)),
  putMany: (store, values) => tx(store, 'readwrite', (os) => { for (const v of values) os.put(v); }),
  async clear() { for (const s of STORES) if (s !== 'files') await tx(s, 'readwrite', (os) => os.clear()); },
};

const B = persistent ? idb : memory;

export const getAll = (store) => (store === 'files' && persistent ? Promise.resolve([]) : B.getAll(store));
export const entries = (store) => B.entries(store);
export const get = (store, key) => B.get(store, key);
export const put = (store, value, key) => B.put(store, value, key);
export const del = (store, key) => B.del(store, key);
export const putMany = (store, values) => B.putMany(store, values);
export const clearLibrary = () => B.clear();

/** file: a File (browser) — ignored in the app, where the track carries its own `uri`. */
export async function addTrack(track, file, artBlob, thumbBlob) {
  await B.put('tracks', track);
  if (file && !persistent) await B.put('files', file, track.id);
  if (artBlob) await B.put('art', artBlob, track.id);
  if (thumbBlob) await B.put('thumbs', thumbBlob, track.id);
}

export async function deleteTrack(id) {
  for (const s of ['tracks', 'art', 'thumbs']) await B.del(s, id);
  if (!persistent) await B.del('files', id);
  const lists = await B.getAll('playlists');
  for (const pl of lists) {
    if (!pl.trackIds.includes(id)) continue;
    pl.trackIds = pl.trackIds.filter((t) => t !== id);
    await B.put('playlists', pl);
  }
}

export const deletePlaylist = (id) => B.del('playlists', id);

export function requestPersistence() {
  if (persistent && navigator.storage && navigator.storage.persist) navigator.storage.persist().catch(() => {});
}

/**
 * Earlier versions (named Aura) copied every imported song into the browser's database.
 * Delete that database so the space is given back.
 */
export function purgeOldLibrary() {
  return new Promise((resolve) => {
    try {
      const req = indexedDB.deleteDatabase('aura-library');
      req.onsuccess = req.onerror = req.onblocked = () => resolve();
    } catch { resolve(); }
  });
}
