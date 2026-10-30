cmake_minimum_required(VERSION 3.22)
project(GrooveCore VERSION 0.1.0 LANGUAGES C CXX)

# ------------------------------------------------------------------------------------------------
# Groove shared audio core. Builds for:
#   desktop (tests, sanitizers, fuzzing)  : cmake -S core -B build -DGROOVE_TESTS=ON
#   Android (via platform/android/jni)    : add_subdirectory(core) from the NDK CMakeLists
#   iOS (via scripts/build-ios-xcframework.sh)
# Dependencies: libFLAC, libsndfile, SQLite. Desktop uses system packages; mobile builds fetch pinned
# sources (GROOVE_FETCH_DEPS=ON) so the exact same versions ship on both platforms.
# ------------------------------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(GROOVE_TESTS "Build unit tests" OFF)
option(GROOVE_FUZZ "Build libFuzzer targets (clang only)" OFF)
option(GROOVE_TOOLS "Build groove-cli" OFF)
option(GROOVE_FETCH_DEPS "Download and build libFLAC/libsndfile/SQLite instead of using system packages" OFF)
option(GROOVE_WERROR "Treat warnings as errors" OFF)
option(GROOVE_SNDFILE_CODECS "With GROOVE_FETCH_DEPS: build libsndfile with Ogg Vorbis/Opus/MP3 (needs those libraries)" OFF)
set(GROOVE_SANITIZE "" CACHE STRING "Sanitizers: address,undefined | thread | memory | hwaddress")

# ---------------------------------------------------------------- dependencies
if(GROOVE_FETCH_DEPS)
  include(FetchContent)
  set(BUILD_SHARED_LIBS OFF)
  # libFLAC 1.4.3 (BSD-3)
  set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
  set(WITH_OGG OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(flac GIT_REPOSITORY https://github.com/xiph/flac.git GIT_TAG 1.4.3 GIT_SHALLOW TRUE)
  # libsndfile 1.2.2 (LGPL-2.1: link dynamically on iOS, see docs/ENGINE.md)
  # Patched: upstream leaks the Vorbis codec state when a malformed Ogg Vorbis file fails to open
  # (found by core/fuzz/fuzz_decoders; see third_party/patches/README.md).
  set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ENABLE_EXTERNAL_LIBS ${GROOVE_SNDFILE_CODECS} CACHE BOOL "" FORCE)  # off on mobile: WAV/AIFF/W64/CAF; FLAC uses libFLAC
  set(ENABLE_MPEG ${GROOVE_SNDFILE_CODECS} CACHE BOOL "" FORCE)
  set(BUILD_REGTEST OFF CACHE BOOL "" FORCE)
  set(GROOVE_SNDFILE_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/patches/libsndfile-1.2.2-vorbis-open-leak.patch)
  FetchContent_Declare(sndfile GIT_REPOSITORY https://github.com/libsndfile/libsndfile.git GIT_TAG 1.2.2 GIT_SHALLOW TRUE
                       PATCH_COMMAND sh -c "git apply --reverse --check '${GROOVE_SNDFILE_PATCH}' 2>/dev/null || git apply '${GROOVE_SNDFILE_PATCH}'")
  # SQLite 3.49.2 amalgamation (public domain), bundled so the old system SQLite on Android 7–10 never matters
  # (the NDK doesn't expose one anyway). Taken from the better-sqlite3 npm package, which ships the untouched
  # amalgamation; pinned by the SHA-256 below (verified when this file was written). Swap for sqlite.org's zip if preferred.
  FetchContent_Declare(sqlite URL https://registry.npmjs.org/better-sqlite3/-/better-sqlite3-11.10.0.tgz
                       URL_HASH SHA256=71e2d66a44dafc35649ff82742cf4908557fc31841108d19703b74b7065c0293)
  FetchContent_MakeAvailable(flac sndfile sqlite)
  add_library(groove_sqlite STATIC ${sqlite_SOURCE_DIR}/deps/sqlite3/sqlite3.c)
  target_include_directories(groove_sqlite PUBLIC ${sqlite_SOURCE_DIR}/deps/sqlite3)
  target_compile_definitions(groove_sqlite PRIVATE SQLITE_THREADSAFE=2 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)
  set(GROOVE_DEP_LIBS FLAC sndfile groove_sqlite)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)
  pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
  find_package(SQLite3 REQUIRED)
  set(GROOVE_DEP_LIBS PkgConfig::FLAC PkgConfig::SNDFILE SQLite::SQLite3)
endif()

# ---------------------------------------------------------------- warnings / sanitizers
add_library(groove_flags INTERFACE)
target_compile_options(groove_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
                       -Woverloaded-virtual -Wnull-dereference -Wformat=2 -Wimplicit-fallthrough)
if(GROOVE_WERROR)
  target_compile_options(groove_flags INTERFACE -Werror)
endif()
if(GROOVE_SANITIZE)
  target_compile_options(groove_flags INTERFACE -fsanitize=${GROOVE_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
  target_link_options(groove_flags INTERFACE -fsanitize=${GROOVE_SANITIZE})
endif()

# ---------------------------------------------------------------- library
add_library(groove_core STATIC
  src/engine/AudioEngine.cpp
  src/engine/Types.cpp
  src/dsp/Equalizer.cpp
  src/io/ByteSource.cpp
  src/decode/DecoderFactory.cpp
  src/decode/FlacDecoder.cpp
  src/decode/SndfileDecoder.cpp
  src/decode/DsdDecoder.cpp
  src/decode/WavStream.cpp
  src/meta/MetadataReader.cpp
  src/library/LibraryDb.cpp
  src/library/LibraryScanner.cpp
  src/library/SmartPlaylist.cpp
  src/output/NullOutput.cpp
)
target_include_directories(groove_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(groove_core PUBLIC ${GROOVE_DEP_LIBS} Threads::Threads PRIVATE groove_flags)
if(GROOVE_SANITIZE)
  target_link_libraries(groove_core PUBLIC groove_flags)
endif()

# ---------------------------------------------------------------- tests / tools / fuzzing
if(GROOVE_TESTS)
  enable_testing()
  add_executable(groove_tests
    tests/main.cpp
    tests/test_ring_eq.cpp
    tests/test_decoders.cpp
    tests/test_metadata.cpp
    tests/test_engine.cpp
    tests/test_library.cpp
    tests/test_wavstream.cpp
    tests/TestUtil.cpp)
  target_include_directories(groove_tests PRIVATE tests)
  target_link_libraries(groove_tests PRIVATE groove_core groove_flags)
  target_compile_definitions(groove_tests PRIVATE GROOVE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME groove_tests COMMAND groove_tests)
endif()

if(GROOVE_TOOLS)
  add_executable(groove-cli tools/groove_cli.cpp)
  target_link_libraries(groove-cli PRIVATE groove_core groove_flags)
endif()

if(GROOVE_FUZZ)
  add_executable(fuzz_metadata fuzz/fuzz_metadata.cpp)
  target_compile_options(fuzz_metadata PRIVATE -fsanitize=fuzzer,address,undefined -g)
  target_link_options(fuzz_metadata PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(fuzz_metadata PRIVATE groove_core)
  add_executable(fuzz_decoders fuzz/fuzz_decoders.cpp)
  target_compile_options(fuzz_decoders PRIVATE -fsanitize=fuzzer,address,undefined -g)
  target_link_options(fuzz_decoders PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_libraries(fuzz_decoders PRIVATE groove_core)
endif()
