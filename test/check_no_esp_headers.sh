#!/bin/sh
# Rule 3, enforced rather than remembered.
#
# `components/core/` may never include an ESP header, and neither may
# `components/ui/` or `components/hal/` — that is what lets all three build
# with plain g++ and be tested in a second instead of a flash cycle. The rule
# is easy to break by accident: one `#include "esp_log.h"` for a quick printf
# and the host build stops working, usually in a commit about something else.
#
# Run from the test directory; takes the repo root as its only argument.
set -e
root="${1:-..}"
fail=0

for dir in core ui hal; do
    hits=$(grep -rn -E '#[[:space:]]*include[[:space:]]*[<"](esp_|freertos/|driver/|nvs|soc/|hal/(gpio|spi)_|Arduino)' \
        "$root/components/$dir" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "components/$dir must not include platform headers:"
        echo "$hits"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "FAIL no_platform_headers"
    exit 1
fi

echo "PASS no_platform_headers"
