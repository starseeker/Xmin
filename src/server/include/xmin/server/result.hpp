#ifndef XMIN_SERVER_RESULT_HPP
#define XMIN_SERVER_RESULT_HPP

#include <string>
#include <utility>
#include <variant>

namespace xmin::server {

enum class ErrorCode {
    io,
    unexpected_eof,
    malformed,
    invalid_argument,
    busy,
};

struct Error {
    ErrorCode code;
    std::string message;
};

template <typename T>
class Result {
public:
    static Result success(T value) { return Result(std::move(value)); }

    static Result failure(ErrorCode code, std::string message)
    {
        return Result(Error{code, std::move(message)});
    }

    explicit operator bool() const noexcept
    {
        return std::holds_alternative<T>(storage_);
    }

    T &value() { return std::get<T>(storage_); }
    const T &value() const { return std::get<T>(storage_); }
    const Error &error() const { return std::get<Error>(storage_); }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    static Result success() { return Result(); }

    static Result failure(ErrorCode code, std::string message)
    {
        return Result(Error{code, std::move(message)});
    }

    explicit operator bool() const noexcept { return !failed_; }
    const Error &error() const { return error_; }

private:
    Result() = default;
    explicit Result(Error error) : failed_(true), error_(std::move(error)) {}

    bool failed_ = false;
    Error error_{ErrorCode::io, {}};
};

} // namespace xmin::server

#endif
