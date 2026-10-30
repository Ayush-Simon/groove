#include "groove/Types.h"

namespace groove {

const char* stateName(PlaybackState s) noexcept {
    switch (s) {
        case PlaybackState::Stopped: return "Stopped";
        case PlaybackState::Loading: return "Loading";
        case PlaybackState::Playing: return "Playing";
        case PlaybackState::Paused: return "Paused";
        case PlaybackState::Error: return "Error";
    }
    return "?";
}

const char* errorName(ErrorCode e) noexcept {
    switch (e) {
        case ErrorCode::None: return "None";
        case ErrorCode::FileNotFound: return "FileNotFound";
        case ErrorCode::Io: return "Io";
        case ErrorCode::Unsupported: return "Unsupported";
        case ErrorCode::Corrupt: return "Corrupt";
        case ErrorCode::DeviceLost: return "DeviceLost";
        case ErrorCode::DeviceOpenFailed: return "DeviceOpenFailed";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::InvalidState: return "InvalidState";
        case ErrorCode::OutOfMemory: return "OutOfMemory";
        case ErrorCode::Internal: return "Internal";
    }
    return "?";
}

}  // namespace groove
