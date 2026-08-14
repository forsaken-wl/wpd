#pragma once
#include <cairo.h>
#include <glib.h>
typedef enum { WPD_SCALE_FILL, WPD_SCALE_FIT, WPD_SCALE_STRETCH, WPD_SCALE_NONE } WpdScaleMode;
gboolean wpd_scale_mode_parse(const gchar *s,WpdScaleMode *out);
cairo_surface_t *wpd_image_load(const gchar *path,GError **error);
cairo_surface_t *wpd_image_render(const gchar *path,gint width,gint height,WpdScaleMode mode,GError **error);
