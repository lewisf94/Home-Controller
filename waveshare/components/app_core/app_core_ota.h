#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start an over-the-air firmware update from an https `url`. The server
 * certificate is verified against the embedded certificate bundle. Runs on its
 * own task, streams the image into the inactive OTA slot, and reboots into it
 * on success. Progress and errors are pushed to the UI via
 * ui_set_ota_status(). Returns false immediately if an update is already
 * running, the url is empty, or the internal memory reserve is too low. */
bool app_core_ota_start(const char *url);

/* Confirm that the running image works. With bootloader rollback enabled, a
 * new OTA image boots as "pending verify". If the device resets before this
 * call, the bootloader returns to the previous image. Call it once the
 * backend has connected. Later calls do nothing. */
void app_core_ota_mark_valid(void);

/* True while an update is in progress (used to block a second start). */
bool app_core_ota_in_progress(void);

#ifdef __cplusplus
}
#endif
