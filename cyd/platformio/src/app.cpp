#include "app.h"
#include "ui.h"

static bool albumMode = false;

void app_init()
{
    albumMode = false;
}

void app_toggle_mode()
{
    albumMode = !albumMode;

    if(albumMode)
        ui_show_album_browser();
    else
        ui_show_now_playing();
}