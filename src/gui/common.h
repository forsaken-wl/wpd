#pragma once
#include "../config.h"
typedef enum {WPD_GUI_SWITCHER,WPD_GUI_CAROUSEL,WPD_GUI_3D} WpdGuiKind;
int wpd_gui_run(WpdConfig *config,WpdGuiKind kind);
