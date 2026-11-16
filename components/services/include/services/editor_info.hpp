// What the screen editor (the hotspot's /screens page) needs from the
// firmware, as JSON (docs/custom-screens.md §8). Board-free: `wifi` streams
// it, tools/screens_json prints it for the editor's dev server, and the
// page never offers a field, an action or a glyph this firmware lacks.
#pragma once
#include <cstddef>
#include <cstdint>
#include "core/layout/fields.hpp"

namespace core::state { struct Frame; }

namespace services {

// Text out, a piece at a time. Each call is a whole JSON fragment.
using EmitFn = void (*)(const char* s, size_t n, void* ctx);

// {"font":..., "icons":[...], "builtins":[...], "actions":[...], "fields":[...]}
void write_editor_info(EmitFn emit, void* ctx);

// [{"id":1,"t":"8230","u":"rpm","n":8234,"v":1}, ...] for `ids` (up to n).
void write_values(const core::state::Frame& f, const core::layout::ShowCtx& c,
                  const uint16_t* ids, size_t n, EmitFn emit, void* ctx);

// "1,2,25" -> ids; how many were read (at most `max`).
size_t parse_ids(const char* s, uint16_t* ids, size_t max);

}  // namespace services
