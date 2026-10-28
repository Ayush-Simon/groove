// Reads tags, artwork and the real stream format from an audio file.
// Nothing is guessed: if a value is not in the file it stays null.

const td = (enc) => new TextDecoder(enc);
const utf8 = td('utf-8');
const latin1 = td('iso-8859-1');

function clean(s) {
  if (s == null) return null;
  s = String(s).replace(/\0+$/g, '').replace(/\0/g, ' ').trim();
  return s.length ? s : null;
}

async function readBytes(blob, offset, length) {
  if (offset >= blob.size || length <= 0) return new Uint8Array(0);
  const end = Math.min(blob.size, offset + length);
  return new Uint8Array(await blob.slice(offset, end).arrayBuffer());
}

const be32 = (b, o) => ((b[o] << 24) >>> 0) + (b[o + 1] << 16) + (b[o + 2] << 8) + b[o + 3];
const le32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)) + ((b[o + 3] << 24) >>> 0);
const be16 = (b, o) => (b[o] << 8) | b[o + 1];
const le16 = (b, o) => b[o] | (b[o + 1] << 8);
const be24 = (b, o) => (b[o] << 16) | (b[o + 1] << 8) | b[o + 2];
const fourcc = (b, o) => String.fromCharCode(b[o], b[o + 1], b[o + 2], b[o + 3]);
const syncsafe = (b, o) => (b[o] << 21) | (b[o + 1] << 14) | (b[o + 2] << 7) | b[o + 3];

function emptyResult(file) {
  return {
    title: null, artist: null, album: null, albumArtist: null,
    year: null, genre: null, trackNo: null, discNo: null,
    codec: null, lossless: null, sampleRate: null, bitDepth: null,
    channels: null, bitrate: null, duration: null,
    art: null, // { data: Uint8Array, mime }
    fileName: file.name || 'Untitled',
  };
}

// ---------- Vorbis comments (FLAC, Ogg) ----------
function applyVorbisComments(b, o, end, r) {
  const vendorLen = le32(b, o); o += 4 + vendorLen;
  if (o + 4 > end) return;
  const count = le32(b, o); o += 4;
  for (let i = 0; i < count && o + 4 <= end; i++) {
    const len = le32(b, o); o += 4;
    if (o + len > end) break;
    const str = utf8.decode(b.subarray(o, o + len)); o += len;
    const eq = str.indexOf('=');
    if (eq < 0) continue;
    const k = str.slice(0, eq).toUpperCase();
    const v = str.slice(eq + 1);
    switch (k) {
      case 'TITLE': r.title ??= clean(v); break;
      case 'ARTIST': r.artist ??= clean(v); break;
      case 'ALBUM': r.album ??= clean(v); break;
      case 'ALBUMARTIST': case 'ALBUM ARTIST': r.albumArtist ??= clean(v); break;
      case 'DATE': case 'YEAR': r.year ??= clean(v)?.slice(0, 4) ?? null; break;
      case 'GENRE': r.genre ??= clean(v); break;
      case 'TRACKNUMBER': r.trackNo ??= parseInt(v, 10) || null; break;
      case 'DISCNUMBER': r.discNo ??= parseInt(v, 10) || null; break;
      case 'METADATA_BLOCK_PICTURE':
        if (!r.art) {
          try {
            const bin = Uint8Array.from(atob(v.trim()), (c) => c.charCodeAt(0));
            parseFlacPicture(bin, 0, bin.length, r);
          } catch { /* ignore broken picture */ }
        }
        break;
    }
  }
}

function parseFlacPicture(b, o, end, r) {
  const type = be32(b, o); o += 4;
  const mimeLen = be32(b, o); o += 4;
  const mime = latin1.decode(b.subarray(o, o + mimeLen)); o += mimeLen;
  const descLen = be32(b, o); o += 4 + descLen;
  o += 16; // width, height, depth, colours
  const dataLen = be32(b, o); o += 4;
  if (o + dataLen > end || dataLen === 0) return;
  // Prefer the front cover (type 3); otherwise take the first picture.
  if (!r.art || type === 3) r.art = { data: b.slice(o, o + dataLen), mime: mime || 'image/jpeg' };
}

// ---------- FLAC ----------
async function parseFlac(file, r) {
  r.codec = 'FLAC'; r.lossless = true;
  let off = 4;
  for (let guard = 0; guard < 128 && off + 4 <= file.size; guard++) {
    const h = await readBytes(file, off, 4);
    const last = (h[0] & 0x80) !== 0;
    const type = h[0] & 0x7f;
    const len = be24(h, 1);
    off += 4;
    if (type === 0 || type === 4 || (type === 6 && !r.art)) {
      if (len > 64 * 1024 * 1024) break;
      const b = await readBytes(file, off, len);
      if (type === 0) {
        // STREAMINFO
        const sr = (b[10] << 12) | (b[11] << 4) | (b[12] >> 4);
        const ch = ((b[12] >> 1) & 0x07) + 1;
        const bps = (((b[12] & 0x01) << 4) | (b[13] >> 4)) + 1;
        const total = (b[13] & 0x0f) * 2 ** 32 + be32(b, 14);
        r.sampleRate = sr || null; r.channels = ch; r.bitDepth = bps;
        if (sr && total) r.duration = total / sr;
      } else if (type === 4) {
        applyVorbisComments(b, 0, b.length, r);
      } else {
        parseFlacPicture(b, 0, b.length, r);
      }
    }
    off += len;
    if (last) {
      if (r.duration && !r.bitrate) r.bitrate = Math.round(((file.size - off) * 8) / r.duration / 1000);
      break;
    }
  }
}

// ---------- ID3v2 ----------
function decodeId3Text(b) {
  if (!b.length) return null;
  const enc = b[0];
  const body = b.subarray(1);
  let s;
  if (enc === 1) s = td('utf-16').decode(body);
  else if (enc === 2) s = td('utf-16be').decode(body);
  else if (enc === 3) s = utf8.decode(body);
  else s = latin1.decode(body);
  return clean(s.split('\0').filter(Boolean)[0] ?? s);
}

function id3Genre(g) {
  if (!g) return null;
  const m = g.match(/^\((\d+)\)/) || g.match(/^(\d+)$/);
  const list = ['Blues', 'Classic Rock', 'Country', 'Dance', 'Disco', 'Funk', 'Grunge', 'Hip-Hop', 'Jazz', 'Metal', 'New Age', 'Oldies', 'Other', 'Pop', 'R&B', 'Rap', 'Reggae', 'Rock', 'Techno', 'Industrial', 'Alternative', 'Ska', 'Death Metal', 'Pranks', 'Soundtrack', 'Euro-Techno', 'Ambient', 'Trip-Hop', 'Vocal', 'Jazz+Funk', 'Fusion', 'Trance', 'Classical', 'Instrumental', 'Acid', 'House', 'Game', 'Sound Clip', 'Gospel', 'Noise', 'Alternative Rock', 'Bass', 'Soul', 'Punk', 'Space', 'Meditative', 'Instrumental Pop', 'Instrumental Rock', 'Ethnic', 'Gothic', 'Darkwave', 'Techno-Industrial', 'Electronic'];
  if (m) return list[parseInt(m[1], 10)] ?? g;
  return g;
}

function parseId3(b, r) {
  // b starts at "ID3"
  const ver = b[3];
  const flags = b[5];
  const size = syncsafe(b, 6);
  let o = 10;
  const end = Math.min(b.length, 10 + size);
  if (flags & 0x40 && ver >= 3) {
    const ext = ver === 4 ? syncsafe(b, o) : be32(b, o) + 4;
    o += ext;
  }
  const v22 = ver === 2;
  const idLen = v22 ? 3 : 4;
  const hdrLen = v22 ? 6 : 10;
  while (o + hdrLen <= end) {
    const id = latin1.decode(b.subarray(o, o + idLen));
    if (!/^[A-Z0-9]+$/.test(id)) break;
    const fsize = v22 ? be24(b, o + 3) : ver === 4 ? syncsafe(b, o + 4) : be32(b, o + 4);
    const fflags = v22 ? 0 : be16(b, o + 8);
    o += hdrLen;
    if (fsize <= 0 || o + fsize > end) break;
    let data = b.subarray(o, o + fsize);
    o += fsize;
    if (ver === 4 && (fflags & 0x0001)) data = data.subarray(4); // data length indicator
    if (fflags & 0x00c0 && ver === 3) continue; // compressed / encrypted: skip
    const key = v22 ? { TT2: 'TIT2', TP1: 'TPE1', TAL: 'TALB', TP2: 'TPE2', TYE: 'TYER', TCO: 'TCON', TRK: 'TRCK', TPA: 'TPOS', PIC: 'APIC' }[id] : id;
    switch (key) {
      case 'TIT2': r.title ??= decodeId3Text(data); break;
      case 'TPE1': r.artist ??= decodeId3Text(data); break;
      case 'TALB': r.album ??= decodeId3Text(data); break;
      case 'TPE2': r.albumArtist ??= decodeId3Text(data); break;
      case 'TDRC': case 'TYER': r.year ??= decodeId3Text(data)?.slice(0, 4) ?? null; break;
      case 'TCON': r.genre ??= id3Genre(decodeId3Text(data)); break;
      case 'TRCK': r.trackNo ??= parseInt(decodeId3Text(data), 10) || null; break;
      case 'TPOS': r.discNo ??= parseInt(decodeId3Text(data), 10) || null; break;
      case 'APIC': {
        if (r.art && r.artIsFront) break;
        const enc = data[0];
        let p = 1, mime;
        if (v22) {
          const fmt = latin1.decode(data.subarray(1, 4)).toLowerCase();
          mime = fmt === 'png' ? 'image/png' : 'image/jpeg';
          p = 4;
        } else {
          const z = data.indexOf(0, 1);
          mime = latin1.decode(data.subarray(1, z)) || 'image/jpeg';
          p = z + 1;
        }
        const picType = data[p]; p += 1;
        // skip description (terminated by 0 or 00 00 for UTF-16)
        if (enc === 1 || enc === 2) {
          while (p + 1 < data.length && !(data[p] === 0 && data[p + 1] === 0)) p += 2;
          p += 2;
        } else {
          while (p < data.length && data[p] !== 0) p++;
          p += 1;
        }
        if (p < data.length) {
          if (!mime.includes('/')) mime = 'image/' + mime.toLowerCase().replace('jpg', 'jpeg');
          r.art = { data: data.slice(p), mime };
          r.artIsFront = picType === 3;
        }
        break;
      }
    }
  }
  return 10 + size + ((flags & 0x10) ? 10 : 0);
}

// ---------- MP3 frame header / Xing ----------
const MP3_BITRATES = {
  // [version][layer] -> kbps table (index 1..14)
  'V1L3': [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
  'V2L3': [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
};
const MP3_RATES = { 3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000] };

async function parseMp3Stream(file, start, r) {
  r.codec = 'MP3'; r.lossless = false;
  const b = await readBytes(file, start, 64 * 1024);
  for (let i = 0; i + 4 < b.length; i++) {
    if (b[i] !== 0xff || (b[i + 1] & 0xe0) !== 0xe0) continue;
    const verBits = (b[i + 1] >> 3) & 0x03; // 3=V1, 2=V2, 0=V2.5
    const layer = (b[i + 1] >> 1) & 0x03;   // 1 = Layer III
    const brIdx = b[i + 2] >> 4;
    const srIdx = (b[i + 2] >> 2) & 0x03;
    if (verBits === 1 || layer !== 1 || brIdx === 0 || brIdx === 15 || srIdx === 3) continue;
    const sr = MP3_RATES[verBits][srIdx];
    const kbps = (verBits === 3 ? MP3_BITRATES.V1L3 : MP3_BITRATES.V2L3)[brIdx];
    const mode = b[i + 3] >> 6; // 3 = mono
    r.sampleRate = sr; r.channels = mode === 3 ? 1 : 2;
    const spf = verBits === 3 ? 1152 : 576;
    const sideInfo = verBits === 3 ? (mode === 3 ? 17 : 32) : (mode === 3 ? 9 : 17);
    const x = i + 4 + sideInfo;
    const tag = fourcc(b, x);
    const audioBytes = file.size - start - i;
    if ((tag === 'Xing' || tag === 'Info') && x + 16 <= b.length) {
      const fl = be32(b, x + 4);
      let p = x + 8;
      let frames = null, bytes = null;
      if (fl & 1) { frames = be32(b, p); p += 4; }
      if (fl & 2) { bytes = be32(b, p); p += 4; }
      if (frames) {
        r.duration = (frames * spf) / sr;
        // "Info" marks a constant-bitrate file: the frame header bitrate is exact.
        r.bitrate = tag === 'Info' && kbps ? kbps : Math.round(((bytes || audioBytes) * 8) / r.duration / 1000);
      }
    } else if (fourcc(b, i + 36) === 'VBRI') {
      const bytes = be32(b, i + 36 + 10);
      const frames = be32(b, i + 36 + 14);
      if (frames) {
        r.duration = (frames * spf) / sr;
        r.bitrate = Math.round((bytes * 8) / r.duration / 1000);
      }
    }
    if (!r.duration && kbps) {
      r.bitrate = kbps;
      r.duration = (audioBytes * 8) / (kbps * 1000);
    }
    return;
  }
}

// ---------- WAV / AIFF ----------
async function parseRiff(file, r) {
  r.codec = 'WAV'; r.lossless = true;
  let off = 12;
  let byteRate = null;
  for (let guard = 0; guard < 256 && off + 8 <= file.size; guard++) {
    const h = await readBytes(file, off, 8);
    const id = fourcc(h, 0);
    const size = le32(h, 4);
    const body = off + 8;
    if (id === 'fmt ') {
      const b = await readBytes(file, body, Math.min(size, 64));
      let fmt = le16(b, 0);
      r.channels = le16(b, 2);
      r.sampleRate = le32(b, 4);
      byteRate = le32(b, 8);
      r.bitDepth = le16(b, 14);
      if (fmt === 0xfffe && size >= 40) fmt = le16(b, 24);
      if (fmt === 3) r.codec = 'WAV (float)';
      else if (fmt !== 1) { r.codec = 'WAV (compressed)'; r.lossless = null; }
    } else if (id === 'data') {
      if (byteRate) r.duration = Math.min(size, file.size - body) / byteRate;
    } else if (id === 'LIST' && size < 1024 * 1024) {
      const b = await readBytes(file, body, size);
      if (fourcc(b, 0) === 'INFO') {
        let p = 4;
        while (p + 8 <= b.length) {
          const sid = fourcc(b, p); const sl = le32(b, p + 4);
          const val = clean(latin1.decode(b.subarray(p + 8, p + 8 + sl)));
          if (sid === 'INAM') r.title ??= val;
          else if (sid === 'IART') r.artist ??= val;
          else if (sid === 'IPRD') r.album ??= val;
          else if (sid === 'ICRD') r.year ??= val?.slice(0, 4) ?? null;
          else if (sid === 'IGNR') r.genre ??= val;
          else if (sid === 'ITRK') r.trackNo ??= parseInt(val, 10) || null;
          p += 8 + sl + (sl & 1);
        }
      }
    } else if ((id === 'id3 ' || id === 'ID3 ') && size < 64 * 1024 * 1024) {
      const b = await readBytes(file, body, size);
      if (fourcc(b, 0).startsWith('ID3')) parseId3(b, r);
    }
    off = body + size + (size & 1);
  }
  if (r.sampleRate && r.bitDepth && r.channels) r.bitrate = Math.round((r.sampleRate * r.bitDepth * r.channels) / 1000);
}

function ieee80(b, o) {
  const exp = ((b[o] & 0x7f) << 8) | b[o + 1];
  const hi = be32(b, o + 2), lo = be32(b, o + 6);
  if (exp === 0 && hi === 0 && lo === 0) return 0;
  return (hi * 2 ** (exp - 16383 - 31)) + (lo * 2 ** (exp - 16383 - 63));
}

async function parseAiff(file, r, isAifc) {
  r.codec = 'AIFF'; r.lossless = true;
  let off = 12;
  for (let guard = 0; guard < 256 && off + 8 <= file.size; guard++) {
    const h = await readBytes(file, off, 8);
    const id = fourcc(h, 0);
    const size = be32(h, 4);
    const body = off + 8;
    if (id === 'COMM') {
      const b = await readBytes(file, body, Math.min(size, 64));
      r.channels = be16(b, 0);
      const frames = be32(b, 2);
      r.bitDepth = be16(b, 6);
      r.sampleRate = Math.round(ieee80(b, 8));
      if (r.sampleRate) r.duration = frames / r.sampleRate;
      if (isAifc && size >= 22) {
        const comp = fourcc(b, 18);
        if (comp !== 'NONE' && comp !== 'sowt' && comp !== 'twos') { r.codec = 'AIFF-C'; r.lossless = null; }
      }
    } else if ((id === 'ID3 ' || id === 'id3 ') && size < 64 * 1024 * 1024) {
      const b = await readBytes(file, body, size);
      if (fourcc(b, 0).startsWith('ID3')) parseId3(b, r);
    } else if (id === 'NAME' && size < 4096) {
      r.title ??= clean(latin1.decode(await readBytes(file, body, size)));
    } else if (id === 'AUTH' && size < 4096) {
      r.artist ??= clean(latin1.decode(await readBytes(file, body, size)));
    }
    off = body + size + (size & 1);
  }
  if (r.sampleRate && r.bitDepth && r.channels) r.bitrate = Math.round((r.sampleRate * r.bitDepth * r.channels) / 1000);
}

// ---------- MP4 / M4A ----------
async function parseMp4(file, r) {
  // Find moov among top-level atoms (it can be at the start or the end).
  let off = 0, moov = null;
  for (let guard = 0; guard < 512 && off + 8 <= file.size; guard++) {
    const h = await readBytes(file, off, 16);
    let size = be32(h, 0);
    const type = fourcc(h, 4);
    let hdr = 8;
    if (size === 1) { size = be32(h, 8) * 2 ** 32 + be32(h, 12); hdr = 16; }
    else if (size === 0) size = file.size - off;
    if (size < hdr) break;
    if (type === 'moov') { moov = { off: off + hdr, len: size - hdr }; break; }
    off += size;
  }
  if (!moov || moov.len > 128 * 1024 * 1024) return;
  const b = await readBytes(file, moov.off, moov.len);

  const children = (start, end) => {
    const out = [];
    let p = start;
    while (p + 8 <= end) {
      let size = be32(b, p);
      const type = fourcc(b, p + 4);
      let hdr = 8;
      if (size === 1) { size = be32(b, p + 8) * 2 ** 32 + be32(b, p + 12); hdr = 16; }
      else if (size === 0) size = end - p;
      if (size < hdr || p + size > end) break;
      out.push({ type, start: p + hdr, end: p + size });
      p += size;
    }
    return out;
  };
  const find = (start, end, path) => {
    let cur = { start, end };
    for (const t of path) {
      const kids = children(cur.start, cur.end);
      const next = kids.find((k) => k.type === t);
      if (!next) return null;
      cur = next;
      if (t === 'meta') cur = { start: cur.start + 4, end: cur.end }; // full box
    }
    return cur;
  };

  // Audio track: first trak whose hdlr is 'soun'
  for (const trak of children(0, b.length).filter((k) => k.type === 'trak')) {
    const hdlr = find(trak.start, trak.end, ['mdia', 'hdlr']);
    if (!hdlr || fourcc(b, hdlr.start + 8) !== 'soun') continue;
    const mdhd = find(trak.start, trak.end, ['mdia', 'mdhd']);
    if (mdhd) {
      const v = b[mdhd.start];
      const ts = v === 1 ? be32(b, mdhd.start + 20) : be32(b, mdhd.start + 12);
      const dur = v === 1 ? be32(b, mdhd.start + 24) * 2 ** 32 + be32(b, mdhd.start + 28) : be32(b, mdhd.start + 16);
      if (ts) r.duration = dur / ts;
    }
    const stsd = find(trak.start, trak.end, ['mdia', 'minf', 'stbl', 'stsd']);
    if (stsd) {
      const e = stsd.start + 8; // skip version/flags + entry count
      const fmt = fourcc(b, e + 4);
      const entryEnd = e + be32(b, e);
      r.channels = be16(b, e + 24);
      r.bitDepth = be16(b, e + 26);
      r.sampleRate = be32(b, e + 32) >>> 16;
      if (fmt === 'alac') {
        r.codec = 'ALAC'; r.lossless = true;
        const cookie = children(e + 36, entryEnd).find((k) => k.type === 'alac');
        if (cookie) {
          const c = cookie.start + 4; // version/flags
          r.bitDepth = b[c + 5];
          r.channels = b[c + 9];
          r.sampleRate = be32(b, c + 20);
        }
      } else if (fmt === 'mp4a') {
        r.codec = 'AAC'; r.lossless = false; r.bitDepth = null;
        const esds = children(e + 36, entryEnd).find((k) => k.type === 'esds');
        if (esds) {
          // DecoderConfigDescriptor (tag 0x04): avgBitrate at +9
          for (let p = esds.start + 4; p < esds.end - 13; p++) {
            if (b[p] === 0x04) {
              let q = p + 1;
              while (b[q] & 0x80) q++;
              q++;
              const avg = be32(b, q + 9);
              if (avg) r.bitrate = Math.round(avg / 1000);
              break;
            }
          }
        }
      } else if (fmt === 'fLaC') { r.codec = 'FLAC'; r.lossless = true; }
      else if (fmt === 'Opus') { r.codec = 'Opus'; r.lossless = false; r.bitDepth = null; }
      else if (fmt === 'ac-3' || fmt === 'ec-3') { r.codec = fmt.toUpperCase(); r.lossless = false; r.bitDepth = null; }
    }
    break;
  }

  const ilst = find(0, b.length, ['udta', 'meta', 'ilst']);
  if (ilst) {
    for (const item of children(ilst.start, ilst.end)) {
      const data = children(item.start, item.end).find((k) => k.type === 'data');
      if (!data) continue;
      const kind = be32(b, data.start) & 0xffffff;
      const payload = b.subarray(data.start + 8, data.end);
      const text = () => clean(utf8.decode(payload));
      switch (item.type) {
        case '©nam': r.title ??= text(); break;
        case '©ART': r.artist ??= text(); break;
        case '©alb': r.album ??= text(); break;
        case 'aART': r.albumArtist ??= text(); break;
        case '©day': r.year ??= text()?.slice(0, 4) ?? null; break;
        case '©gen': r.genre ??= text(); break;
        case 'gnre': if (payload.length >= 2) r.genre ??= id3Genre(String(be16(payload, 0) - 1)); break;
        case 'trkn': if (payload.length >= 4) r.trackNo ??= be16(payload, 2) || null; break;
        case 'disk': if (payload.length >= 4) r.discNo ??= be16(payload, 2) || null; break;
        case 'covr':
          if (!r.art && payload.length) r.art = { data: payload.slice(), mime: kind === 14 ? 'image/png' : 'image/jpeg' };
          break;
      }
    }
  }
  if (!r.bitrate && r.duration) r.bitrate = Math.round((file.size * 8) / r.duration / 1000);
}

// ---------- Ogg (Vorbis / Opus) ----------
async function parseOgg(file, r) {
  const b = await readBytes(file, 0, 512 * 1024);
  const packets = [];
  let cur = [];
  let p = 0;
  while (p + 27 <= b.length && packets.length < 3) {
    if (fourcc(b, p) !== 'OggS') break;
    const nseg = b[p + 26];
    const segs = b.subarray(p + 27, p + 27 + nseg);
    let q = p + 27 + nseg;
    for (const len of segs) {
      cur.push(b.subarray(q, q + len));
      q += len;
      if (len < 255) {
        const total = cur.reduce((n, a) => n + a.length, 0);
        const pk = new Uint8Array(total);
        let w = 0;
        for (const a of cur) { pk.set(a, w); w += a.length; }
        packets.push(pk);
        cur = [];
      }
    }
    p = q;
  }
  const id = packets[0];
  if (!id) return;
  if (latin1.decode(id.subarray(0, 8)) === 'OpusHead') {
    r.codec = 'Opus'; r.lossless = false;
    r.channels = id[9];
    r.sampleRate = 48000; // Opus always decodes at 48 kHz
    if (packets[1] && latin1.decode(packets[1].subarray(0, 8)) === 'OpusTags') applyVorbisComments(packets[1], 8, packets[1].length, r);
  } else if (id[0] === 1 && latin1.decode(id.subarray(1, 7)) === 'vorbis') {
    r.codec = 'Vorbis'; r.lossless = false;
    r.channels = id[11];
    r.sampleRate = le32(id, 12);
    const nominal = le32(id, 20);
    if (nominal > 0 && nominal < 2e6) r.bitrate = Math.round(nominal / 1000);
    if (packets[1] && packets[1][0] === 3) applyVorbisComments(packets[1], 7, packets[1].length, r);
  } else if (id[0] === 0x7f && latin1.decode(id.subarray(1, 5)) === 'FLAC') {
    r.codec = 'FLAC'; r.lossless = true;
    const si = id.subarray(13 + 4);
    r.sampleRate = (si[10] << 12) | (si[11] << 4) | (si[12] >> 4);
    r.channels = ((si[12] >> 1) & 7) + 1;
    r.bitDepth = (((si[12] & 1) << 4) | (si[13] >> 4)) + 1;
  }
  // Duration from the granule position of the last page.
  const tail = await readBytes(file, Math.max(0, file.size - 64 * 1024), 64 * 1024);
  for (let i = tail.length - 27; i >= 0; i--) {
    if (tail[i] === 0x4f && fourcc(tail, i) === 'OggS') {
      const gp = le32(tail, i + 6) + le32(tail, i + 10) * 2 ** 32;
      const rate = r.codec === 'Opus' ? 48000 : r.sampleRate;
      if (rate && gp > 0) {
        r.duration = gp / rate;
        if (!r.bitrate) r.bitrate = Math.round((file.size * 8) / r.duration / 1000);
      }
      break;
    }
  }
}

export async function readMetadata(file) {
  const r = emptyResult(file);
  try {
    const head = await readBytes(file, 0, 16);
    const magic = fourcc(head, 0);
    if (magic === 'fLaC') await parseFlac(file, r);
    else if (magic === 'RIFF' && fourcc(head, 8) === 'WAVE') await parseRiff(file, r);
    else if (magic === 'FORM' && (fourcc(head, 8) === 'AIFF' || fourcc(head, 8) === 'AIFC')) await parseAiff(file, r, fourcc(head, 8) === 'AIFC');
    else if (magic === 'OggS') await parseOgg(file, r);
    else if (fourcc(head, 4) === 'ftyp') await parseMp4(file, r);
    else if (magic.startsWith('ID3')) {
      const hdr = await readBytes(file, 0, 10);
      const size = syncsafe(hdr, 6) + 10;
      const tag = await readBytes(file, 0, Math.min(size + 10, 64 * 1024 * 1024));
      const audioStart = parseId3(tag, r);
      // ID3 can also prefix FLAC/AAC; check what follows.
      const after = await readBytes(file, audioStart, 4);
      if (fourcc(after, 0) === 'fLaC') {
        const view = { size: file.size - audioStart, slice: (a, z) => file.slice(a + audioStart, z + audioStart) };
        await parseFlac(view, r);
      } else {
        await parseMp3Stream(file, audioStart, r);
      }
    } else if (head[0] === 0xff && (head[1] & 0xe0) === 0xe0) {
      await parseMp3Stream(file, 0, r);
    }
  } catch (err) {
    console.warn('Could not fully read metadata for', file.name, err);
  }
  delete r.artIsFront;
  if (!r.codec) {
    const ext = (file.name || '').split('.').pop().toLowerCase();
    r.codec = ext ? ext.toUpperCase() : null;
  }
  return r;
}

// ---------- Display helpers ----------
export function qualityInfo(t) {
  // Returns { badge, detail } using the same thresholds Apple Music uses:
  // Lossless = up to 24-bit/48 kHz, Hi-Res Lossless = above 48 kHz.
  const khz = t.sampleRate ? +(t.sampleRate / 1000).toFixed(1) : null;
  const rate = khz ? `${khz % 1 === 0 ? khz.toFixed(0) : khz} kHz` : null;
  if (t.lossless) {
    const badge = t.sampleRate > 48000 ? 'Hi-Res Lossless' : 'Lossless';
    const parts = [t.bitDepth ? `${t.bitDepth}-bit` : null, rate].filter(Boolean).join('/');
    return { badge, detail: [parts, t.codec].filter(Boolean).join(' ') };
  }
  const parts = [t.codec, t.bitrate ? `${t.bitrate} kbps` : null].filter(Boolean).join(' · ');
  return { badge: null, detail: parts || '' };
}
