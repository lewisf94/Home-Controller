#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start an over-the-air firmware update from `url` (http or https -- https is
 * verified against the embedded certificate bundle). Runs on its own task,
 * streams the image into the inactive OTA slot, and reboots into it on success.
 * Progress and errors are pushed to the UI via ui_set_ota_status(). Returns
 * false immediately if an update is already running or the url is empty. */
bool app_core_ota_start(const char *url);

/* True while an update is in progress (used to block a second start). */
bool app_core_ota_in_progress(void);

#ifdef __cplusplus
}
#endif
