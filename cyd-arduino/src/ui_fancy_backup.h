#ifndef UI_H
#define UI_H

void ui_init();
void ui_update(); // Call this in loop()
void ui_show_album_browser();
void ui_show_now_playing();

// Memory Management
void ui_suspend_sprite();
void ui_resume_sprite();

#endif // UI_H