#include "common/status.h"

namespace lsm {

std::string Status::ToString() const {
    if (code_ == StatusCode::Ok) {
        return "OK";
    }

    std::string prefix;
    switch (code_) {
        case StatusCode::NotFound:
            prefix = "NotFound";
            break;
        case StatusCode::AlreadyExists:
            prefix = "AlreadyExists";
            break;
        case StatusCode::InvalidArgument:
            prefix = "InvalidArgument";
            break;
        case StatusCode::IOError:
            prefix = "IOError";
            break;
        case StatusCode::Corruption:
            prefix = "Corruption";
            break;
        case StatusCode::NotSupported:
            prefix = "NotSupported";
            break;
        case StatusCode::InternalError:
            prefix = "InternalError";
            break;
        default:
            prefix = "UnknownError";
            break;
    }

    if (message_.empty()) {
        return prefix;
    }
    return prefix + ": " + message_;
}

std::ostream& operator<<(std::ostream& os, const Status& status) {
    os << status.ToString();
    return os;
}

} // namespace lsm
