// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Forward the dash's K-line adapter frames (ESP-NOW) to the host. Call once,
// after the coprocessor is up; it does nothing until the host starts Wi-Fi.
void dash_espnow_init(void);

#ifdef __cplusplus
}
#endif
