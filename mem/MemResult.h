#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace Mem {

enum class ErrorCode {
    InvalidArgument,
    NotConnected,
    NoTarget,
    TargetChanged,
    ConnectionChanged,
    ConnectionPoisoned,
    Timeout,
    CancelRequested,
    CompletionUnknown,
    NoScanSession,
    ScanSessionChanged,
    ProtocolError,
    Unsupported,
    PermissionDenied,
    InternalError,
};

inline const char* errorCodeName(ErrorCode code) {
    switch (code) {
        case ErrorCode::InvalidArgument:     return "invalid_argument";
        case ErrorCode::NotConnected:       return "not_connected";
        case ErrorCode::NoTarget:           return "no_target";
        case ErrorCode::TargetChanged:      return "target_changed";
        case ErrorCode::ConnectionChanged:  return "connection_changed";
        case ErrorCode::ConnectionPoisoned: return "connection_poisoned";
        case ErrorCode::Timeout:             return "timeout";
        case ErrorCode::CancelRequested:     return "cancel_requested";
        case ErrorCode::CompletionUnknown:   return "completion_unknown";
        case ErrorCode::NoScanSession:      return "no_scan_session";
        case ErrorCode::ScanSessionChanged: return "scan_session_changed";
        case ErrorCode::ProtocolError:       return "protocol_error";
        case ErrorCode::Unsupported:         return "unsupported";
        case ErrorCode::PermissionDenied:    return "permission_denied";
        case ErrorCode::InternalError:       return "internal_error";
        default:                             return "internal_error";
    }
}

struct Error {
    ErrorCode code = ErrorCode::InternalError;
    std::string message;
    bool retryable = false;
};

template <typename T>
class Result {
public:
    static Result success(T value, uint64_t durationMs = 0) {
        return Result(std::move(value), std::nullopt, durationMs);
    }

    static Result failure(Error error, uint64_t durationMs = 0) {
        return Result(std::nullopt, std::move(error), durationMs);
    }

    static Result failure(ErrorCode code,
                          std::string message,
                          bool retryable = false,
                          uint64_t durationMs = 0) {
        return failure(Error{code, std::move(message), retryable}, durationMs);
    }

    bool ok() const {
        return value_.has_value() && !error_.has_value();
    }

    const T& value() const {
        assert(ok());
        return *value_;
    }

    T& value() {
        assert(ok());
        return *value_;
    }

    const Error& error() const {
        assert(!ok() && error_.has_value());
        return *error_;
    }

    uint64_t durationMs() const {
        return durationMs_;
    }

private:
    Result(std::optional<T> value,
           std::optional<Error> error,
           uint64_t durationMs)
        : value_(std::move(value)),
          error_(std::move(error)),
          durationMs_(durationMs) {
        assert(value_.has_value() != error_.has_value());
    }

    std::optional<T> value_;
    std::optional<Error> error_;
    uint64_t durationMs_ = 0;
};

} // namespace Mem
