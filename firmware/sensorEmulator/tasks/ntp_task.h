/**
 * @file ntp_task.h
 * @brief SNTP time synchronisation + timezone management — Phase 9.
 *
 * Provides a FreeRTOS task that:
 *   - Applies the stored POSIX TZ string from NVS at boot (UTC0 if absent).
 *   - Starts SNTP whenever the WiFi STA link comes up.
 *   - Stops SNTP and clears the synced flag when the STA link drops.
 *
 * Consumers (web_server) call ntp_is_synced() to populate the
 * @c ntp_synced field of the WebSocket status push.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create and start the NTP synchronisation task.
 *
 * Applies the POSIX TZ string from NVS immediately (before entering the
 * task loop), registers the SNTP sync callback, then creates a FreeRTOS
 * task (priority 2, stack 3 KiB) that monitors the WiFi EventGroup.
 *
 * Must be called once from @c setup() after @c wifi_manager_init().
 */
void ntp_task_init(void);

/**
 * @brief Return @c true if SNTP has successfully synchronised the system
 *        clock at least once since the last reboot.
 *
 * Thread-safe — the underlying flag is @c volatile and updated only from
 * the SNTP callback; there is no mutex needed for a single @c bool read.
 */
bool ntp_is_synced(void);

#ifdef __cplusplus
}
#endif
