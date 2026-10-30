# libgroove.so for Android (NDK r26+). Used from Gradle:
#   android { externalNativeBuild { cmake { path = file("../platform/android/jni/CMakeLists.txt") } } }
# or directly:
#   cmake -S platform/android/jni -B build/android-arm64 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.22)
project(groove_android LANGUAGES C CXX)

set(GROOVE_FETCH_DEPS ON CACHE BOOL "" FORCE)  # libFLAC / libsndfile (patched) / SQLite from pinned sources
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../core ${CMAKE_BINARY_DIR}/groove-core)

add_library(groove SHARED GrooveJni.cpp AAudioOutput.cpp)
target_link_libraries(groove PRIVATE groove_core groove_flags aaudio log android)
target_compile_options(groove PRIVATE -fvisibility=hidden -ffunction-sections -fdata-sections -Werror=unguarded-availability)
# Newer-API symbols are weak and every use is wrapped in __builtin_available (see AAudioOutput.cpp).
target_compile_definitions(groove PRIVATE __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__)
target_link_options(groove PRIVATE
  -Wl,--gc-sections
  -Wl,--exclude-libs,ALL           # only JNI_OnLoad is exported; everything else stays internal
  -Wl,-z,max-page-size=16384)       # 16 KB page support (required for new apps on Android 15+ devices)

# HWAddressSanitizer build for on-device memory checking (arm64, Android 14+):
#   -DGROOVE_SANITIZE=hwaddress   and add `android:debuggable` + the hwasan wrap.sh, see docs/ENGINE.md
