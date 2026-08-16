// The one thing every driver returns.
//
// Deliberately not `esp_err_t`. `core/` and `ui/` build on the host, and a
// driver interface that leaks its transport's error type into a header drags
// the whole IDF up the stack behind it.
//
// The contract every implementation signs, from rule 5: a driver reports
// health, it does not decide policy. Never abort(), never block forever,
// never retry in a tight loop. Return one of these and let the service and
// the keeper task decide what it means.
#pragma once
#include <cstdint>

namespace hal {

enum class Status : int8_t {
    Ok = 0,
    NotPresent,     // nothing answered; probably not fitted
    Timeout,
    BadReply,       // answered, but with something impossible
    Busy,
    Unsupported,
    IoError,
};

inline bool ok(Status s) { return s == Status::Ok; }

const char* status_name(Status s);

}  // namespace hal
