#pragma once
#include <glib.h>
#include <gdk/gdk.h>

typedef struct {
  gchar *config_dir, *papers_dir, *cache_dir, *state_dir, *runtime_dir;
  gchar *socket_path, *state_file, *theme_file, *matugen_command, *transition;
  guint duration_ms, fps;
} WpdConfig;

typedef struct {
  gboolean band, light;
  gint skew, radius, dim, spacing, selected_width, side_width, height, border;
  GdkRGBA background_color, foreground_color, selected_color, border_color, shadow_color;
} SwitcherConfig;

WpdConfig *wpd_config_new(void);
void wpd_config_free(WpdConfig *c);
void wpd_switcher_config_load(const WpdConfig *c, SwitcherConfig *out);
const gchar *wpd_theme_get(const WpdConfig *c);
gboolean wpd_theme_set(const WpdConfig *c, const gchar *theme, GError **error);
gboolean wpd_transition_set(const WpdConfig *c, const gchar *transition, GError **error);
