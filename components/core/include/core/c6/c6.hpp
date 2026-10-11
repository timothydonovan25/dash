// When to replace the C6's firmware (§11.7, docs/fusion.md's neighbour:
// the radio co-processor).
//
// The dash carries one C6 image, in the P4's `slave_fw` partition, and the
// rule is simple: the C6 runs exactly that image. Not "at least this
// version" -- two builds can share a version number and differ in what they
// do, which is how the C6 ended up on a 2.12.0 nobody had built from this
// repo. So each build is identified by its *build id*: the first four bytes
// of its ELF's SHA-256, which the IDF stamps into every app description.
// The C6 reports its own in the `build` field of the firmware-version RPC
// (demo_p4/c6_cp/main/slave_control.c); the P4 reads the image's out of
// slave_fw. Different, and the image is pushed.
//
// Two guards:
//  - each image is tried once. If the C6 will not take it, the dash does
//    not restart forever trying; a new image in slave_fw gets a new try.
//  - a dev slot may not move the C6 across a major version, because stable
//    shares the C6 and may not speak the new protocol.
//
// Pure logic. Reading the partition, the RPC and NVS are the service's job.
#pragma once
#include <cstddef>
#include <cstdint>

namespace core::c6 {

struct FwId {
    uint32_t major = 0, minor = 0, patch = 0;
    // 0 means unknown: a C6 build from before build ids, which reports -1.
    int32_t  build = 0;
    bool     valid = false;
};

// The id from the first four bytes of an app's ELF SHA-256: positive, and
// never 0, because the RPC turns a 0 into -1 ("absent"). The C6 computes
// the same thing in C; keep the two in step.
int32_t build_id(const uint8_t sha[4]);

// "2.12.8" -> 2, 12, 8. False for anything else.
bool parse_version(const char* s, FwId& out);

enum class Action : uint8_t {
    None = 0,      // the C6 runs the image already (or there is no image)
    Push,          // different build: send it
    AlreadyTried,  // different, but this image was sent before and did not take
    DevRefused,    // would cross a major version from a dev slot
};
const char* action_name(Action a);

// `tried_build` is the build id last pushed, from NVS, 0 if none.
Action decide(const FwId& running, const FwId& image, int32_t tried_build, bool running_dev);

}  // namespace core::c6
