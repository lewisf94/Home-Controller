#ifndef UI_H
#define UI_H

void ui_init();
void ui_update(); // Call this in loop()
void ui_show_album_browser();
void ui_show_now_playing();

// Overlays — call from input layer to trigger transient HUD
void ui_show_volume_hud(int pct, bool muted);

// Memory Management
void ui_suspend_sprite();
void ui_resume_sprite();

#endif // UI_H