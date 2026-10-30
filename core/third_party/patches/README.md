# Patches to third-party code

Applied automatically by `core/CMakeLists.txt` when `GROOVE_FETCH_DEPS=ON` (Android/iOS builds).

| Patch | Upstream | Why |
|---|---|---|
| `libsndfile-1.2.2-vorbis-open-leak.patch` | libsndfile 1.2.2, `src/ogg_vorbis.c` | `vorbis_read_header()` calls `vorbis_info_init()` and can then fail, but `ogg_vorbis_open()` only registers `vorbis_close()` after the headers were read, so a malformed Ogg Vorbis file leaks ~6 KB per open attempt. Found by `core/fuzz/fuzz_decoders` under LeakSanitizer. The patch registers the close handler first; `vorbis_close()` is safe on a zero-initialised codec state. Worth sending upstream. |

Desktop builds that use the system libsndfile (`GROOVE_FETCH_DEPS=OFF`) still carry the upstream bug;
the fuzzer then needs `LSAN_OPTIONS=suppressions=core/fuzz/lsan-system-libsndfile.supp`.
