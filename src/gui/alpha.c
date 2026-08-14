#include "alpha.h"
#include <gtk-layer-shell.h>
#include <gtk/gtk.h>

static gboolean escape_gray(GtkWidget *widget,GdkEventKey *event,gpointer data) {
  (void)widget;(void)data;
  if (event->keyval!=GDK_KEY_Escape) return FALSE;
  gtk_main_quit();
  return TRUE;
}

static void quit_if_running(GtkWidget *widget,gpointer data) {
  (void)widget;(void)data;
  if (gtk_main_level()>0) gtk_main_quit();
}

int wpd_alpha_run(void) {
  GtkWidget *window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_decorated(GTK_WINDOW(window),FALSE);
  gtk_layer_init_for_window(GTK_WINDOW(window));
  gtk_layer_set_layer(GTK_WINDOW(window),GTK_LAYER_SHELL_LAYER_OVERLAY);
  gtk_layer_set_anchor(GTK_WINDOW(window),GTK_LAYER_SHELL_EDGE_LEFT,TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(window),GTK_LAYER_SHELL_EDGE_RIGHT,TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(window),GTK_LAYER_SHELL_EDGE_TOP,TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(window),GTK_LAYER_SHELL_EDGE_BOTTOM,TRUE);
  gtk_layer_set_exclusive_zone(GTK_WINDOW(window),0);
  gtk_layer_set_keyboard_mode(GTK_WINDOW(window),GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
  gtk_layer_set_namespace(GTK_WINDOW(window),"wpd-alpha-1-1-1");

  GtkCssProvider *css=gtk_css_provider_new();
  gtk_css_provider_load_from_data(css,"window { background: #808080; }",-1,NULL);
  gtk_style_context_add_provider(gtk_widget_get_style_context(window),
    GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
  g_signal_connect(window,"key-press-event",G_CALLBACK(escape_gray),NULL);
  g_signal_connect(window,"destroy",G_CALLBACK(quit_if_running),NULL);
  gtk_widget_show(window);
  gtk_main();
  gtk_widget_destroy(window);
  return 0;
}
