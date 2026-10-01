#pragma once

#include <string>
#include <string_view>
#include <iostream>

namespace lsm {

enum class StatusCode {
    Ok = 0,
    NotFound = 1,
    AlreadyExists = 2,
    InvalidArgument = 3,
    IOError = 4,
    Corruption = 5,
    NotSupported = 6,
    InternalError = 7
};

class Status {
public:
    Status() : code_(StatusCode::Ok), message_("") {}
    Status(StatusCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    // Factory methods
    static Status Ok() {
        return Status(StatusCode::Ok, "");
    }

    static Status NotFound(std::string message) {
        return Status(StatusCode::NotFound, std::move(message));
    }

    static Status AlreadyExists(std::string message) {
        return Status(StatusCode::AlreadyExists, std::move(message));
    }

    static Status InvalidArgument(std::string message) {
        return Status(StatusCode::InvalidArgument, std::move(message));
    }

    static Status IOError(std::string message) {
        return Status(StatusCode::IOError, std::move(message));
    }

    static Status Corruption(std::string message) {
        return Status(StatusCode::Corruption, std::move(message));
    }

    static Status NotSupported(std::string message) {
        return Status(StatusCode::NotSupported, std::move(message));
    }

    static Status InternalError(std::string message) {
        return Status(StatusCode::InternalError, std::move(message));
    }

    // Inspection
    [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
    [[nodiscard]] StatusCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    [[nodiscard]] std::string ToString() const;

    bool operator==(const Status& other) const noexcept {
        return code_ == other.code_ && message_ == other.message_;
    }

    bool operator!=(const Status& other) const noexcept {
        return !(*this == other);
    }

private:
    StatusCode code_;
    std::string message_;
};

std::ostream& operator<<(std::ostream& os, const Status& status);

} // namespace lsm
