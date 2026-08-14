#include "renderer.h"
#include <gtk-layer-shell.h>

typedef struct { GtkWidget *window; cairo_surface_t *current, *next; gint w, h; } Output;
struct _WpdRenderer {
  GPtrArray *outputs;
  guint duration, fps, timer;
  gint64 started;
  WpdTransition transition;
};

static void output_free(gpointer ptr) {
  Output *o = ptr;
  if (o->current) cairo_surface_destroy(o->current);
  if (o->next) cairo_surface_destroy(o->next);
  gtk_widget_destroy(o->window);
  g_free(o);
}

static gboolean draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
  Output *o = data;
  WpdRenderer *r = g_object_get_data(G_OBJECT(widget), "wpd-renderer");
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_paint(cr);
  if (o->next && r->timer) {
    double p = (g_get_monotonic_time() - r->started) / (r->duration * 1000.0);
    wpd_transition_draw(cr, o->current, o->next, r->transition, p, o->w, o->h);
  } else if (o->current) {
    cairo_set_source_surface(cr, o->current, 0, 0); cairo_paint(cr);
  }
  return FALSE;
}

static gboolean tick(gpointer data) {
  WpdRenderer *r = data;
  double p = (g_get_monotonic_time() - r->started) / (r->duration * 1000.0);
  for (guint i = 0; i < r->outputs->len; i++)
    gtk_widget_queue_draw(((Output *)g_ptr_array_index(r->outputs, i))->window);
  if (p < 1) return G_SOURCE_CONTINUE;
  for (guint i = 0; i < r->outputs->len; i++) {
    Output *o = g_ptr_array_index(r->outputs, i);
    if (o->current) cairo_surface_destroy(o->current);
    o->current = o->next; o->next = NULL;
  }
  r->timer = 0;
  return G_SOURCE_REMOVE;
}

WpdRenderer *wpd_renderer_new(guint duration, guint fps) {
  WpdRenderer *r = g_new0(WpdRenderer, 1);
  r->outputs = g_ptr_array_new_with_free_func(output_free);
  r->duration = duration; r->fps = fps;
  GdkDisplay *display = gdk_display_get_default();
  gint count = gdk_display_get_n_monitors(display);
  for (gint i = 0; i < count; i++) {
    GdkMonitor *monitor = gdk_display_get_monitor(display, i);
    GdkRectangle rect; gdk_monitor_get_geometry(monitor, &rect);
    Output *o = g_new0(Output, 1); o->w = rect.width; o->h = rect.height;
    o->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(o->window), FALSE);
    gtk_widget_set_app_paintable(o->window, TRUE);
    gtk_widget_set_size_request(o->window, o->w, o->h);
    gtk_layer_init_for_window(GTK_WINDOW(o->window));
    gtk_layer_set_layer(GTK_WINDOW(o->window), GTK_LAYER_SHELL_LAYER_BACKGROUND);
    gtk_layer_set_monitor(GTK_WINDOW(o->window), monitor);
    gtk_layer_set_anchor(GTK_WINDOW(o->window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(o->window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(o->window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(o->window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(o->window), 0);
    gtk_layer_set_namespace(GTK_WINDOW(o->window), "wpd");
    g_object_set_data(G_OBJECT(o->window), "wpd-renderer", r);
    g_signal_connect(o->window, "draw", G_CALLBACK(draw), o);
    gtk_widget_show(o->window);
    g_ptr_array_add(r->outputs, o);
  }
  return r;
}

void wpd_renderer_free(WpdRenderer *r) {
  if (!r) return;
  if (r->timer) g_source_remove(r->timer);
  g_ptr_array_free(r->outputs, TRUE); g_free(r);
}

gboolean wpd_renderer_set(WpdRenderer *r, const gchar *path, WpdScaleMode mode,
                          WpdTransition transition, GError **error) {
  GPtrArray *loaded = g_ptr_array_new_with_free_func((GDestroyNotify)cairo_surface_destroy);
  for (guint i = 0; i < r->outputs->len; i++) {
    Output *o = g_ptr_array_index(r->outputs, i);
    cairo_surface_t *surface = wpd_image_render(path, o->w, o->h, mode, error);
    if (!surface) { g_ptr_array_free(loaded, TRUE); return FALSE; }
    g_ptr_array_add(loaded, surface);
  }
  if (r->timer) { g_source_remove(r->timer); r->timer = 0; }
  for (guint i = 0; i < r->outputs->len; i++) {
    Output *o = g_ptr_array_index(r->outputs, i);
    if (o->next) cairo_surface_destroy(o->next);
    o->next = g_ptr_array_index(loaded, i);
  }
  g_ptr_array_set_free_func(loaded, NULL); g_ptr_array_free(loaded, TRUE);
  r->transition = transition; r->started = g_get_monotonic_time();
  r->timer = g_timeout_add(MAX(4, 1000 / r->fps), tick, r);
  return TRUE;
}
