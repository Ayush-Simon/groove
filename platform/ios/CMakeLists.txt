# libGrooveCore.a for iOS (wrapped into an xcframework by scripts/build-ios-xcframework.sh).
cmake_minimum_required(VERSION 3.22)
project(groove_ios LANGUAGES C CXX OBJCXX)

set(GROOVE_FETCH_DEPS ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core ${CMAKE_BINARY_DIR}/groove-core)

add_library(GrooveCore STATIC GrooveCore/GRVEngine.mm GrooveCore/CoreAudioOutput.mm)
target_include_directories(GrooveCore PUBLIC GrooveCore)
target_compile_options(GrooveCore PRIVATE -fobjc-arc)
target_link_libraries(GrooveCore PUBLIC groove_core "-framework AudioToolbox" "-framework AVFoundation" "-framework Foundation")

# Public header for Swift (bridging header / module map imports GRVEngine.h only).
file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/GrooveCore/include)
configure_file(GrooveCore/GRVEngine.h ${CMAKE_CURRENT_SOURCE_DIR}/GrooveCore/include/GRVEngine.h COPYONLY)
