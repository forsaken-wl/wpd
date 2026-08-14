#include "layouts.h"
#include "common.h"
int wpd_grid_run(WpdConfig *c){return wpd_gui_run(c,WPD_GUI_GRID);}
int wpd_stack_run(WpdConfig *c){return wpd_gui_run(c,WPD_GUI_STACK);}
int wpd_filmstrip_run(WpdConfig *c){return wpd_gui_run(c,WPD_GUI_FILMSTRIP);}
int wpd_roots_run(WpdConfig *c){return wpd_gui_run(c,WPD_GUI_ROOTS);}
