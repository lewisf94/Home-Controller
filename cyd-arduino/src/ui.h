#ifndef UI_H
#define UI_H

void ui_init();
void ui_update(); // Call this in loop()
void ui_show_album_browser();
void ui_show_now_playing();
void ui_toggle_view();

// Overlays — call from input layer to trigger transient HUD
void ui_show_volume_hud(int pct, bool muted);

// View queries
bool ui_is_now_playing();

// Play the album currently centred in the browser and switch to now-playing
void ui_play_centered_album();

// Memory Management
void ui_suspend_sprite();
void ui_resume_sprite();

#endif // UI_H