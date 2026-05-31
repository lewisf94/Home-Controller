#ifndef LVGL_APP_H
#define LVGL_APP_H

// LVGL is single-threaded. The render/input loop (core 1) and the Spotify
// task (which publishes track info / album art) serialise all LVGL access
// through this recursive mutex. Mirrors the ESP-IDF build's lvgl_port_lock().
void lvgl_lock();
void lvgl_unlock();

#endif // LVGL_APP_H
