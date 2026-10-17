// Marking a function whose stack frame is big on purpose.
//
// The firmware builds our components with -Wframe-larger-than (the limit
// is in cmake/dash_stack.cmake) under -Werror, because a large local is
// invisible to the host tests and overflows a 3-8 kB task stack only at run
// time: 1.26's LogicState did that to the demo. A function that genuinely
// needs a big frame is wrapped in these, with the reason beside it, and its
// task's stack is sized for it:
//
//     DASH_BIG_FRAME_BEGIN   // ui::App lives here; start_render sizes for it
//     void render_task(void*) { ... }
//     DASH_BIG_FRAME_END
//
// No ESP header, so core/ and ui/ can use it; on the host it is harmless.
#pragma once

#if defined(__GNUC__)
#define DASH_BIG_FRAME_BEGIN \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wframe-larger-than=\"")
#define DASH_BIG_FRAME_END _Pragma("GCC diagnostic pop")
#else
#define DASH_BIG_FRAME_BEGIN
#define DASH_BIG_FRAME_END
#endif
