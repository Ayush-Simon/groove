# Cross-compiles for Android arm64 with a stock LLVM (clang ≥ 20) plus an Android NDK *sysroot* — for machines
# where the full NDK can't be installed. With a normal NDK install use $NDK/build/cmake/android.toolchain.cmake.
#   cmake -S platform/android/jni -B build/android-arm64 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=scripts/android-clang-toolchain.cmake -DGROOVE_NDK_PREBUILT=<ndk>/toolchains/llvm/prebuilt/<host>
set(CMAKE_SYSTEM_NAME Linux)          # bionic is Linux; the target triple below makes clang define __ANDROID__
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(GROOVE_ANDROID_API 26 CACHE STRING "minSdk for the native library (AAudio needs 26)")
if(NOT GROOVE_NDK_PREBUILT)
  set(GROOVE_NDK_PREBUILT "$ENV{GROOVE_NDK_PREBUILT}")
endif()
set(GROOVE_NDK_PREBUILT "${GROOVE_NDK_PREBUILT}" CACHE PATH "NDK toolchains/llvm/prebuilt/<host> directory (sysroot + lib/clang)")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES GROOVE_NDK_PREBUILT GROOVE_ANDROID_API)
file(GLOB _res "${GROOVE_NDK_PREBUILT}/lib/clang/*")
list(GET _res 0 _resdir)
set(_t "--target=aarch64-linux-android${GROOVE_ANDROID_API} --sysroot=${GROOVE_NDK_PREBUILT}/sysroot -resource-dir ${_resdir}")
find_program(CMAKE_C_COMPILER NAMES clang-20 clang-21 clang REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang++-20 clang++-21 clang++ REQUIRED)
set(CMAKE_C_FLAGS_INIT "${_t} -fPIC -fstack-protector-strong -D_FORTIFY_SOURCE=2")
set(CMAKE_CXX_FLAGS_INIT "${_t} -fPIC -fstack-protector-strong -D_FORTIFY_SOURCE=2 -stdlib=libc++")
set(_l "-fuse-ld=lld -rtlib=compiler-rt -unwindlib=libunwind -static-libstdc++ -Wl,--build-id=sha1 -Wl,-z,max-page-size=16384 -Wl,--no-undefined -Wl,-z,relro,-z,now")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_l}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_l}")
set(CMAKE_FIND_ROOT_PATH "${GROOVE_NDK_PREBUILT}/sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ANDROID TRUE)
