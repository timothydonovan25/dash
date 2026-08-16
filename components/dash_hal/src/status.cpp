#include "hal/status.hpp"

namespace hal {

const char* status_name(Status s) {
    switch (s) {
        case Status::Ok:          return "ok";
        case Status::NotPresent:  return "absent";
        case Status::Timeout:     return "timeout";
        case Status::BadReply:    return "bad reply";
        case Status::Busy:        return "busy";
        case Status::Unsupported: return "unsupported";
        case Status::IoError:     return "io error";
    }
    return "?";
}

}  // namespace hal
