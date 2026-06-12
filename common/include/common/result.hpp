#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// result.hpp — Result<T, E> monad
//
// std::variant<T, E> is ambiguous when T == E (e.g., Result<string, string>).
// We use a tagged struct instead: one bool flag + two optionals.
// This is slightly heavier but unambiguous and fully constexpr-compatible.
// ─────────────────────────────────────────────────────────────────────────────

#include <optional>
#include <stdexcept>
#include <string>

// ─── Primary template: Result<T, E> ──────────────────────────────────────────

template <typename T, typename E = std::string>
class Result {
public:
    [[nodiscard]] static Result Ok(T value) {
        Result r;
        r.is_ok_  = true;
        r.value_  = std::move(value);
        return r;
    }

    [[nodiscard]] static Result Err(E error) {
        Result r;
        r.is_ok_  = false;
        r.error_  = std::move(error);
        return r;
    }

    [[nodiscard]] bool ok()  const noexcept { return is_ok_; }
    [[nodiscard]] bool err() const noexcept { return !is_ok_; }
    [[nodiscard]] explicit operator bool() const noexcept { return is_ok_; }

    [[nodiscard]] T& value() {
        if (!is_ok_) throw std::runtime_error("Result::value() called on Err");
        return value_.value();
    }
    [[nodiscard]] const T& value() const {
        if (!is_ok_) throw std::runtime_error("Result::value() called on Err");
        return value_.value();
    }

    [[nodiscard]] E& error() {
        if (is_ok_) throw std::runtime_error("Result::error() called on Ok");
        return error_.value();
    }
    [[nodiscard]] const E& error() const {
        if (is_ok_) throw std::runtime_error("Result::error() called on Ok");
        return error_.value();
    }

    [[nodiscard]] T& operator*()              { return value(); }
    [[nodiscard]] const T& operator*() const  { return value(); }
    [[nodiscard]] T* operator->()             { return &value(); }
    [[nodiscard]] const T* operator->() const { return &value(); }

private:
    bool              is_ok_{false};
    std::optional<T>  value_;
    std::optional<E>  error_;
    Result() = default;
};

// ─── Specialisation: Result<void, E> ─────────────────────────────────────────

template <typename E>
class Result<void, E> {
public:
    [[nodiscard]] static Result Ok() {
        Result r;
        r.is_ok_ = true;
        return r;
    }

    [[nodiscard]] static Result Err(E error) {
        Result r;
        r.is_ok_  = false;
        r.error_  = std::move(error);
        return r;
    }

    [[nodiscard]] bool ok()  const noexcept { return is_ok_; }
    [[nodiscard]] bool err() const noexcept { return !is_ok_; }
    [[nodiscard]] explicit operator bool() const noexcept { return is_ok_; }

    [[nodiscard]] E& error() {
        if (is_ok_) throw std::runtime_error("Result::error() called on Ok");
        return error_.value();
    }
    [[nodiscard]] const E& error() const {
        if (is_ok_) throw std::runtime_error("Result::error() called on Ok");
        return error_.value();
    }

private:
    bool              is_ok_{false};
    std::optional<E>  error_;
    Result() = default;
};

// ─── Convenience alias ────────────────────────────────────────────────────────
using VoidResult = Result<void, std::string>;
