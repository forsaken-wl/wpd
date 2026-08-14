#pragma once
#include <cairo.h>
#include <glib.h>
typedef enum {
  WPD_TRANS_FADE, WPD_TRANS_WIPE, WPD_TRANS_SLIDE, WPD_TRANS_GROW,
  WPD_TRANS_CENTER, WPD_TRANS_CORNER, WPD_TRANS_INFECTION,
  WPD_TRANS_RADIAL, WPD_TRANS_DIAMOND, WPD_TRANS_BLINDS,
  WPD_TRANS_CHECKER, WPD_TRANS_WAVE, WPD_TRANS_CURTAIN,
  WPD_TRANS_CLOCK, WPD_TRANS_DISSOLVE, WPD_TRANS_SWEEP,
  WPD_TRANS_CURVE_CORNER,
  WPD_TRANS_COUNT
} WpdTransition;
#define WPD_TRANS_BASE_MASK 0xff
#define WPD_TRANS_MOD_INVERT (1u<<8)
#define WPD_TRANS_MOD_RIGHT (1u<<9)
#define WPD_TRANS_MOD_BOTTOM (1u<<10)
#define WPD_TRANS_MOD_VERTICAL (1u<<11)
WpdTransition wpd_transition_parse(const char *name);
const char *wpd_transition_name(WpdTransition t);
gboolean wpd_transition_valid(const char *name);
const char *const *wpd_transition_names(gsize *count);
void wpd_transition_draw(cairo_t *cr,cairo_surface_t *old,cairo_surface_t *next,WpdTransition t,double p,int w,int h);
