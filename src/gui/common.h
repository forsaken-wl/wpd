#pragma once
#include "../config.h"
typedef enum {WPD_GUI_SWITCHER,WPD_GUI_CAROUSEL,WPD_GUI_3D,
              WPD_GUI_GRID,WPD_GUI_STACK,WPD_GUI_FILMSTRIP,
              WPD_GUI_ROOTS} WpdGuiKind;
int wpd_gui_run(WpdConfig *config,WpdGuiKind kind);
