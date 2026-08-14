#pragma once
#include "../config.h"
typedef enum {WPD_GUI_SWITCHER,WPD_GUI_CAROUSEL,WPD_GUI_CONCEPT,WPD_GUI_CONCEPT_V2} WpdGuiKind;
int wpd_gui_run(WpdConfig *config,WpdGuiKind kind);
