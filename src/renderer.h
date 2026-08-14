#pragma once
#include <gtk/gtk.h>
#include "image.h"
#include "transitions.h"
typedef struct _WpdRenderer WpdRenderer;
WpdRenderer *wpd_renderer_new(guint duration_ms,guint fps);
void wpd_renderer_free(WpdRenderer *r);
gboolean wpd_renderer_set(WpdRenderer *r,const gchar *path,WpdScaleMode mode,WpdTransition transition,GError **error);
void wpd_renderer_begin_video(WpdRenderer *r,WpdScaleMode mode);
void wpd_renderer_set_video_frame(WpdRenderer *r,cairo_surface_t *frame);
