// Result<T>: value-or-error without exceptions crossing module boundaries.
// C++17 (std::expected is C++23 and not available on every NDK/Xcode toolchain we target).
#pragma once

#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "groove/Types.h"

namespace groove {

struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;
};

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}            // NOLINT(implicit)
    Result(Error err) : v_(std::in_place_index<1>, std::move(err)) {}            // NOLINT(implicit)
    static Result fail(ErrorCode c, std::string msg = {}) { return Result(Error{c, std::move(msg)}); }

    bool ok() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    // Precondition: ok(). Callers check ok() first; std::get would throw otherwise, so these use get_if.
    T& value() & noexcept { return *std::get_if<0>(&v_); }
    const T& value() const& noexcept { return *std::get_if<0>(&v_); }
    T&& value() && noexcept { return std::move(*std::get_if<0>(&v_)); }
    T* operator->() noexcept { return std::get_if<0>(&v_); }

    const Error& error() const noexcept {
        static const Error none{ErrorCode::None, {}};
        const Error* e = std::get_if<1>(&v_);
        return e ? *e : none;
    }
    ErrorCode code() const noexcept { return ok() ? ErrorCode::None : error().code; }

private:
    std::variant<T, Error> v_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error err) : err_(std::move(err)), ok_(false) {}                      // NOLINT(implicit)
    static Result fail(ErrorCode c, std::string msg = {}) { return Result(Error{c, std::move(msg)}); }
    static Result success() { return Result(); }

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    const Error& error() const noexcept { return err_; }
    ErrorCode code() const noexcept { return ok_ ? ErrorCode::None : err_.code; }

private:
    Error err_{ErrorCode::None, {}};
    bool ok_ = true;
};

}  // namespace groove
