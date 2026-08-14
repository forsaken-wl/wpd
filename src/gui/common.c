#include "common.h"
#include "../image.h"
#include "../ipc.h"
#include "../wallpaper.h"
#include <gtk-layer-shell.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { gint refs; gchar *path; cairo_surface_t *thumb; } Item;
typedef struct {
  WpdConfig *config;
  WpdGuiKind kind;
  GtkWidget *window, *area;
  GPtrArray *items;
  gint selected;
  SwitcherConfig style;
  guint animation_timer;
  gint64 animation_last_frame;
  double animation_offset, animation_velocity;
} Ui;
typedef struct { gchar *path, *cache; Item *item; } ThumbJob;
typedef struct { gint index; double position; } RenderEntry;

static gint render_entry_compare(gconstpointer a, gconstpointer b) {
  const RenderEntry *left=a, *right=b;
  double difference=fabs(right->position)-fabs(left->position);
  return difference<0 ? -1 : difference>0 ? 1 : 0;
}

static Item *item_ref(Item *item) {
  g_atomic_int_inc(&item->refs);
  return item;
}

static void item_unref(gpointer data) {
  Item *item = data;
  if (!g_atomic_int_dec_and_test(&item->refs)) return;
  g_free(item->path);
  if (item->thumb) cairo_surface_destroy(item->thumb);
  g_free(item);
}

static void thumb_job_free(ThumbJob *job) {
  g_free(job->path); g_free(job->cache); item_unref(job->item); g_free(job);
}

static void thumb_worker(GTask *task, gpointer source, gpointer task_data,
                         GCancellable *cancel) {
  (void)source; (void)cancel;
  ThumbJob *job = task_data;
  GError *error = NULL;
  cairo_surface_t *surface = NULL;
  if (g_file_test(job->cache, G_FILE_TEST_EXISTS))
    surface = cairo_image_surface_create_from_png(job->cache);
  if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    if (surface) cairo_surface_destroy(surface);
    surface = wpd_image_render(job->path, 480, 300, WPD_SCALE_FILL, &error);
    if (surface) cairo_surface_write_to_png(surface, job->cache);
  }
  if (surface)
    g_task_return_pointer(task, surface, (GDestroyNotify)cairo_surface_destroy);
  else
    g_task_return_error(task, error);
}

static void thumb_done(GObject *source, GAsyncResult *result, gpointer data) {
  (void)data;
  ThumbJob *job = g_task_get_task_data(G_TASK(result));
  Item *item = job->item;
  GError *error = NULL;
  cairo_surface_t *surface = g_task_propagate_pointer(G_TASK(result), &error);
  Ui *ui = g_object_get_data(source, "wpd-ui");
  if (surface) {
    item->thumb = cairo_surface_reference(surface);
    cairo_surface_destroy(surface);
  }
  g_clear_error(&error);
  if (ui) gtk_widget_queue_draw(ui->area);
}

static void request_thumb(Ui *ui, Item *item) {
  GStatBuf file_stat;
  gint64 stamp = !g_stat(item->path, &file_stat) ? file_stat.st_mtime : 0;
  gchar *identity = g_strdup_printf("%s:%" G_GINT64_FORMAT, item->path, stamp);
  gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity, -1);
  ThumbJob *job = g_new0(ThumbJob, 1);
  job->path = g_strdup(item->path);
  job->cache = g_build_filename(ui->config->cache_dir, sum, NULL);
  job->item = item_ref(item);
  g_free(identity); g_free(sum);
  GTask *task = g_task_new(ui->area, NULL, thumb_done, NULL);
  g_object_set_data(G_OBJECT(ui->area), "wpd-ui", ui);
  g_task_set_task_data(task, job, (GDestroyNotify)thumb_job_free);
  g_task_run_in_thread(task, thumb_worker);
  g_object_unref(task);
}

static void apply_selected(Ui *ui) {
  if (!ui->items->len) return;
  Item *item = g_ptr_array_index(ui->items, ui->selected);
  gchar *request = g_strdup_printf("IMAGE\tfill\t%s", item->path);
  gchar *reply = NULL; GError *error = NULL;
  if (!wpd_ipc_send(ui->config->socket_path, request, &reply, &error))
    g_printerr("wpd: %s\n", error->message);
  else if (!g_str_has_prefix(reply, "OK"))
    g_printerr("wpd: %s\n", reply);
  else
    gtk_main_quit();
  g_clear_error(&error); g_free(reply); g_free(request);
}

static gboolean animation_tick(gpointer data) {
  Ui *ui = data;
  gint64 now = g_get_monotonic_time();
  double elapsed = MIN(.05,(now-ui->animation_last_frame)/1000000.0);
  ui->animation_last_frame = now;
  double acceleration = -190.0*ui->animation_offset-28.0*ui->animation_velocity;
  ui->animation_velocity += acceleration*elapsed;
  ui->animation_offset += ui->animation_velocity*elapsed;
  if (fabs(ui->animation_offset) < .35 && fabs(ui->animation_velocity) < 2.0) {
    ui->animation_offset = 0;
    ui->animation_velocity = 0;
    ui->animation_timer = 0;
    gtk_widget_queue_draw(ui->area);
    return G_SOURCE_REMOVE;
  }
  gtk_widget_queue_draw(ui->area);
  return G_SOURCE_CONTINUE;
}

static void select_delta(Ui *ui, gint delta) {
  if (!ui->items->len) return;
  ui->selected=(ui->selected+delta+(gint)ui->items->len)%ui->items->len;
  ui->animation_offset += delta*ui->style.spacing;
  double cycle=ui->items->len*ui->style.spacing;
  while (ui->animation_offset>cycle/2) ui->animation_offset-=cycle;
  while (ui->animation_offset< -cycle/2) ui->animation_offset+=cycle;
  if (!ui->animation_timer) {
    ui->animation_last_frame = g_get_monotonic_time();
    ui->animation_timer = g_timeout_add(16,animation_tick,ui);
  }
}

static gboolean key_press(GtkWidget *widget, GdkEventKey *event, gpointer data) {
  (void)widget; Ui *ui = data;
  if (ui->kind==WPD_GUI_CONCEPT) {
    GtkAllocation area; gtk_widget_get_allocation(ui->area,&area);
    gint cell_width=MAX(120,ui->style.side_width)+28;
    gint columns=MAX(1,(area.width-60)/cell_width);
    gint delta=0;
    if (event->keyval==GDK_KEY_Left || event->keyval==GDK_KEY_h) delta=-1;
    else if (event->keyval==GDK_KEY_Right || event->keyval==GDK_KEY_j) delta=1;
    else if (event->keyval==GDK_KEY_Up || event->keyval==GDK_KEY_k) delta=-columns;
    else if (event->keyval==GDK_KEY_Down || event->keyval==GDK_KEY_l) delta=columns;
    else if (event->keyval==GDK_KEY_Return || event->keyval==GDK_KEY_KP_Enter)
      apply_selected(ui);
    else if (event->keyval==GDK_KEY_Escape) gtk_main_quit();
    else return FALSE;
    if (delta) {
      ui->selected=CLAMP(ui->selected+delta,0,(gint)ui->items->len-1);
      gtk_widget_queue_draw(ui->area);
    }
    return TRUE;
  }
  if (event->keyval == GDK_KEY_Left) select_delta(ui, -1);
  else if (event->keyval == GDK_KEY_Right) select_delta(ui, 1);
  else if (event->keyval == GDK_KEY_h) select_delta(ui,-1);
  else if (event->keyval == GDK_KEY_j) select_delta(ui,1);
  else if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
    apply_selected(ui);
  else if (event->keyval == GDK_KEY_Escape) gtk_main_quit();
  else return FALSE;
  return TRUE;
}

static gboolean scroll(GtkWidget *widget, GdkEventScroll *event, gpointer data) {
  (void)widget;
  Ui *ui=data;
  gint delta=event->direction==GDK_SCROLL_UP || event->delta_y<0 ? -1 : 1;
  if (ui->kind==WPD_GUI_CONCEPT) {
    ui->selected=CLAMP(ui->selected+delta,0,(gint)ui->items->len-1);
    gtk_widget_queue_draw(ui->area);
  } else select_delta(ui,delta);
  return TRUE;
}

static void card_path(cairo_t *cr, double x, double y, double w, double h,
                      double skew, double radius) {
  double r = MIN(radius, MIN(w, h) / 5.0);
  cairo_move_to(cr, x + skew + r, y);
  cairo_line_to(cr, x + w - r, y);
  cairo_curve_to(cr, x+w, y, x+w, y, x+w-r*.4, y+r);
  cairo_line_to(cr, x + w - skew + r*.4, y + h - r);
  cairo_curve_to(cr, x+w-skew, y+h, x+w-skew, y+h, x+w-skew-r, y+h);
  cairo_line_to(cr, x + r, y + h);
  cairo_curve_to(cr, x, y+h, x, y+h, x+r*.4, y+h-r);
  cairo_line_to(cr, x + skew - r*.4, y + r);
  cairo_curve_to(cr, x+skew, y, x+skew, y, x+skew+r, y);
  cairo_close_path(cr);
}

static void paint_card(cairo_t *cr, Item *item, double x, double y,
                       double w, double h, Ui *ui, gboolean selected) {
  card_path(cr,x+3,y+5,w,h,ui->style.skew,ui->style.radius);
  GdkRGBA shadow=ui->style.shadow_color;
  cairo_set_source_rgba(cr,shadow.red,shadow.green,shadow.blue,
                         shadow.alpha*(selected?1.0:.65)); cairo_fill(cr);
  card_path(cr, x, y, w, h, ui->style.skew, ui->style.radius);
  cairo_save(cr); cairo_clip(cr);
  if (item->thumb) {
    double sw = cairo_image_surface_get_width(item->thumb);
    double sh = cairo_image_surface_get_height(item->thumb);
    double scale = MAX(w / sw, h / sh);
    cairo_translate(cr, x + (w-sw*scale)/2, y + (h-sh*scale)/2);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, item->thumb, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr),CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr),CAIRO_EXTEND_PAD);
    cairo_paint(cr);
  } else {
    if (ui->style.light) cairo_set_source_rgb(cr,.88,.89,.91);
    else cairo_set_source_rgb(cr,.10,.11,.14);
    cairo_paint(cr);
  }
  cairo_restore(cr);
  if (!selected && ui->style.dim > 0) {
    card_path(cr, x, y, w, h, ui->style.skew, ui->style.radius);
    cairo_set_source_rgba(cr, 0, 0, 0, ui->style.dim / 100.0); cairo_fill(cr);
  }
  card_path(cr, x, y, w, h, ui->style.skew, ui->style.radius);
  cairo_set_line_width(cr, ui->style.border);
  GdkRGBA line=selected?ui->style.selected_color:ui->style.border_color;
  cairo_set_source_rgba(cr,line.red,line.green,line.blue,line.alpha);
  cairo_stroke(cr);
}

static void perspective_path(cairo_t *cr,double x,double y,double w,double h,double tilt) {
  double inset=MIN(h*.28,fabs(tilt)*h*.22);
  if (tilt<0) {
    cairo_move_to(cr,x,y+inset); cairo_line_to(cr,x+w,y);
    cairo_line_to(cr,x+w,y+h); cairo_line_to(cr,x,y+h-inset);
  } else {
    cairo_move_to(cr,x,y); cairo_line_to(cr,x+w,y+inset);
    cairo_line_to(cr,x+w,y+h-inset); cairo_line_to(cr,x,y+h);
  }
  cairo_close_path(cr);
}

static void paint_3d_card(cairo_t *cr,Item *item,double x,double y,double w,double h,
                          double tilt,Ui *ui,gboolean selected) {
  perspective_path(cr,x+5,y+7,w,h,tilt);
  GdkRGBA shadow=ui->style.shadow_color;
  cairo_set_source_rgba(cr,shadow.red,shadow.green,shadow.blue,shadow.alpha);
  cairo_fill(cr);
  perspective_path(cr,x,y,w,h,tilt); cairo_save(cr); cairo_clip(cr);
  if (item->thumb) {
    double sw=cairo_image_surface_get_width(item->thumb);
    double sh=cairo_image_surface_get_height(item->thumb);
    double scale=MAX(w/sw,h/sh);
    cairo_translate(cr,x+(w-sw*scale)/2,y+(h-sh*scale)/2);
    cairo_scale(cr,scale,scale); cairo_set_source_surface(cr,item->thumb,0,0);
    cairo_pattern_set_filter(cairo_get_source(cr),CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr),CAIRO_EXTEND_PAD); cairo_paint(cr);
  } else {
    GdkRGBA background=ui->style.background_color;
    cairo_set_source_rgba(cr,background.red,background.green,background.blue,1);
    cairo_paint(cr);
  }
  cairo_restore(cr);
  if (!selected && ui->style.dim>0) {
    perspective_path(cr,x,y,w,h,tilt);
    cairo_set_source_rgba(cr,0,0,0,ui->style.dim/100.0); cairo_fill(cr);
  }
  perspective_path(cr,x,y,w,h,tilt); cairo_set_line_width(cr,ui->style.border);
  GdkRGBA line=selected?ui->style.selected_color:ui->style.border_color;
  cairo_set_source_rgba(cr,line.red,line.green,line.blue,line.alpha); cairo_stroke(cr);
}

static gboolean draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
  Ui *ui = data; GtkAllocation area; gtk_widget_get_allocation(widget, &area);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE); cairo_set_source_rgba(cr,0,0,0,0);
  cairo_paint(cr); cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  if (!ui->items->len) {
    GdkRGBA foreground=ui->style.foreground_color;
    cairo_set_source_rgba(cr,foreground.red,foreground.green,foreground.blue,
                          foreground.alpha);
    cairo_move_to(cr,40,60);
    cairo_show_text(cr, "No wallpapers in the WPD papers directory"); return FALSE;
  }
  if (ui->kind==WPD_GUI_CONCEPT) {
    if (ui->style.band) {
      GdkRGBA background=ui->style.background_color;
      cairo_set_source_rgba(cr,background.red,background.green,background.blue,
                            background.alpha); cairo_paint(cr);
    }
    double card_width=MAX(120,ui->style.side_width);
    double card_height=ui->style.height;
    double gap=28, top=42, label_height=24;
    gint columns=MAX(1,(gint)((area.width-60)/(card_width+gap)));
    gint visible_rows=MAX(1,(gint)((area.height-top-32)/(card_height+label_height+gap)));
    gint selected_row=ui->selected/columns;
    gint first_row=MAX(0,selected_row-visible_rows/2);
    gint total_rows=((gint)ui->items->len+columns-1)/columns;
    first_row=MIN(first_row,MAX(0,total_rows-visible_rows));
    double grid_width=columns*card_width+(columns-1)*gap;
    double start_x=(area.width-grid_width)/2;
    for (gint row=first_row;row<MIN(total_rows,first_row+visible_rows);row++) {
      for (gint column=0;column<columns;column++) {
        gint index=row*columns+column;
        if (index>=(gint)ui->items->len) break;
        double x=start_x+column*(card_width+gap);
        double y=top+(row-first_row)*(card_height+label_height+gap);
        paint_card(cr,g_ptr_array_index(ui->items,index),x,y,card_width,card_height,
                   ui,index==ui->selected);
        gchar *name=g_path_get_basename(((Item*)g_ptr_array_index(ui->items,index))->path);
        GdkRGBA foreground=ui->style.foreground_color;
        cairo_set_source_rgba(cr,foreground.red,foreground.green,foreground.blue,
                              foreground.alpha);
        cairo_set_font_size(cr,13); cairo_move_to(cr,x,y+card_height+18);
        cairo_save(cr); cairo_rectangle(cr,x,y+card_height,card_width,label_height);
        cairo_clip(cr); cairo_show_text(cr,name); cairo_restore(cr); g_free(name);
      }
    }
    return FALSE;
  }
  if (ui->kind==WPD_GUI_CONCEPT_V2) {
    double panel_w=MIN(area.width-80.0,1120.0), panel_h=ui->style.height*2.1;
    double panel_x=(area.width-panel_w)/2, panel_y=(area.height-panel_h)/2;
    if (ui->style.band) {
      GdkRGBA background=ui->style.background_color;
      cairo_rectangle(cr,panel_x,panel_y,panel_w,panel_h);
      cairo_set_source_rgba(cr,background.red,background.green,background.blue,
                            background.alpha); cairo_fill(cr);
    }
    cairo_save(cr); cairo_rectangle(cr,panel_x,panel_y,panel_w,panel_h); cairo_clip(cr);
    gint count=(gint)ui->items->len; double offset=ui->animation_offset/ui->style.spacing;
    RenderEntry *entries=g_new(RenderEntry,count);
    for (gint index=0;index<count;index++) {
      double position=index-ui->selected;
      while (position>count/2.0) position-=count;
      while (position< -count/2.0) position+=count;
      position+=offset;
      if (fabs(position+count)<fabs(position)) position+=count;
      if (fabs(position-count)<fabs(position)) position-=count;
      entries[index]=(RenderEntry){index,position};
    }
    qsort(entries,count,sizeof(RenderEntry),render_entry_compare);
    for (gint slot=0;slot<count;slot++) {
      double position=entries[slot].position;
      if (fabs(position)>3.35) continue;
      double angle=CLAMP(position*.58,-1.35,1.35);
      double depth=MAX(.18,cos(angle));
      double scale=.46+.54*depth;
      double width=ui->style.selected_width*scale;
      double height=ui->style.height*(.70+.30*depth);
      double x=area.width/2+sin(angle)*panel_w*.39-width/2;
      double y=area.height/2-height/2+(1-depth)*42;
      gboolean selected=fabs(position)<.48;
      paint_3d_card(cr,g_ptr_array_index(ui->items,entries[slot].index),x,y,width,
                    height,sin(angle),ui,selected);
      if (selected) {
        gchar *name=g_path_get_basename(((Item*)g_ptr_array_index(ui->items,
                                          entries[slot].index))->path);
        GdkRGBA foreground=ui->style.foreground_color;
        cairo_set_source_rgba(cr,foreground.red,foreground.green,foreground.blue,
                              foreground.alpha); cairo_set_font_size(cr,17);
        cairo_move_to(cr,area.width/2-strlen(name)*4.3,panel_y+panel_h-18);
        cairo_show_text(cr,name); g_free(name);
      }
    }
    g_free(entries); cairo_restore(cr); return FALSE;
  }
  double deck_w = 4*ui->style.spacing+ui->style.side_width+76;
  double deck_h = ui->style.height+28;
  double deck_x = (area.width-deck_w)/2, deck_y = (area.height-deck_h)/2;
  if (ui->kind == WPD_GUI_SWITCHER && ui->style.band) {
    cairo_rectangle(cr,deck_x,deck_y,deck_w,deck_h);
    GdkRGBA background=ui->style.background_color;
    cairo_set_source_rgba(cr,background.red,background.green,background.blue,
                          background.alpha);
    cairo_fill(cr);
  }
  cairo_save(cr);
  if (ui->kind == WPD_GUI_SWITCHER) {
    cairo_rectangle(cr,deck_x,deck_y,deck_w,deck_h);
    cairo_clip(cr);
  }
  gint base_range = ui->kind == WPD_GUI_SWITCHER ? 2 : 3;
  gint count=(gint)ui->items->len;
  double offset=ui->animation_offset/ui->style.spacing;
  RenderEntry *entries=g_new(RenderEntry,count);
  for (gint index=0;index<count;index++) {
    double distance=index-ui->selected;
    while (distance>count/2.0) distance-=count;
    while (distance< -count/2.0) distance+=count;
    double position=distance+offset;
    if (fabs(position+count)<fabs(position)) position+=count;
    if (fabs(position-count)<fabs(position)) position-=count;
    entries[index]=(RenderEntry){index,position};
  }
  qsort(entries,count,sizeof(RenderEntry),render_entry_compare);
  for (gint slot=0;slot<count;slot++) {
    gint index=entries[slot].index;
    double position=entries[slot].position;
    if (fabs(position)>base_range+1.5) continue;
    double focus = 1.0-MIN(1.0,fabs(position));
    gboolean selected = focus > .5;
    double width = ui->style.side_width +
      (ui->style.selected_width-ui->style.side_width)*focus;
    double height = ui->style.height;
    if (ui->kind == WPD_GUI_CAROUSEL) {
      double scale = .55 + 1.20*focus;
      width = ui->style.selected_width * scale; height *= scale;
    }
    double x = area.width/2.0 + position*ui->style.spacing - width/2;
    double y = area.height/2.0 - height/2;
    paint_card(cr, g_ptr_array_index(ui->items,index), x,y,width,height,ui,selected);
    if (selected && ui->kind == WPD_GUI_CAROUSEL) {
      gchar *name = g_path_get_basename(((Item*)g_ptr_array_index(ui->items,index))->path);
      GdkRGBA foreground=ui->style.foreground_color;
      cairo_set_source_rgba(cr,foreground.red,foreground.green,foreground.blue,
                            foreground.alpha);
      cairo_set_font_size(cr,18);
      cairo_move_to(cr, area.width/2-strlen(name)*4.5, y+height+32);
      cairo_show_text(cr,name); g_free(name);
    }
  }
  g_free(entries);
  cairo_restore(cr);
  return FALSE;
}

static gboolean click(GtkWidget *widget, GdkEventButton *event, gpointer data) {
  Ui *ui = data; GtkAllocation area; gtk_widget_get_allocation(widget, &area);
  if (ui->kind==WPD_GUI_CONCEPT) {
    double card_width=MAX(120,ui->style.side_width), card_height=ui->style.height;
    double gap=28, top=42, label_height=24;
    gint columns=MAX(1,(gint)((area.width-60)/(card_width+gap)));
    gint visible_rows=MAX(1,(gint)((area.height-top-32)/(card_height+label_height+gap)));
    gint selected_row=ui->selected/columns;
    gint total_rows=((gint)ui->items->len+columns-1)/columns;
    gint first_row=MIN(MAX(0,selected_row-visible_rows/2),MAX(0,total_rows-visible_rows));
    double grid_width=columns*card_width+(columns-1)*gap;
    double start_x=(area.width-grid_width)/2;
    gint column=(gint)((event->x-start_x)/(card_width+gap));
    gint row=(gint)((event->y-top)/(card_height+label_height+gap));
    if (column>=0 && column<columns && row>=0 && row<visible_rows) {
      double local_x=event->x-(start_x+column*(card_width+gap));
      double local_y=event->y-(top+row*(card_height+label_height+gap));
      gint index=(first_row+row)*columns+column;
      if (local_x<=card_width && local_y<=card_height &&
          index<(gint)ui->items->len) {
        if (index==ui->selected) apply_selected(ui);
        else { ui->selected=index; gtk_widget_queue_draw(ui->area); }
      }
    }
    return TRUE;
  }
  if (ui->kind==WPD_GUI_CONCEPT_V2) {
    gint count=(gint)ui->items->len, closest=0; double best=G_MAXDOUBLE;
    double offset=ui->animation_offset/ui->style.spacing;
    for (gint distance=-MIN(3,count);distance<=MIN(3,count);distance++) {
      double position=distance+offset;
      double angle=CLAMP(position*.58,-1.35,1.35);
      double center=area.width/2+sin(angle)*MIN(area.width-80.0,1120.0)*.39;
      double delta=fabs(event->x-center);
      if (delta<best) { best=delta; closest=distance; }
    }
    if (!closest) apply_selected(ui); else select_delta(ui,closest);
    return TRUE;
  }
  gint range = ui->kind == WPD_GUI_SWITCHER ? 2 : 3, closest = 0;
  double best = G_MAXDOUBLE;
  for (gint distance=-range; distance<=range; distance++) {
    double delta = fabs(event->x-(area.width/2.0+distance*ui->style.spacing+
                                  ui->animation_offset));
    if (delta < best) { best = delta; closest = distance; }
  }
  if (!closest) apply_selected(ui); else select_delta(ui, closest);
  return TRUE;
}

int wpd_gui_run(WpdConfig *config, WpdGuiKind kind) {
  Ui ui = {0}; ui.config=config; ui.kind=kind;
  wpd_switcher_config_load(config,&ui.style);
  g_mkdir_with_parents(config->cache_dir,0700);
  GPtrArray *paths = wpd_wallpapers_scan(config->papers_dir);
  ui.items = g_ptr_array_new_with_free_func(item_unref);
  for (guint i=0; i<paths->len; i++) {
    Item *item=g_new0(Item,1); item->refs=1;
    item->path=g_strdup(g_ptr_array_index(paths,i));
    g_ptr_array_add(ui.items,item);
  }
  g_ptr_array_free(paths,TRUE);
  ui.window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_decorated(GTK_WINDOW(ui.window),FALSE);
  gtk_widget_set_app_paintable(ui.window,TRUE);
  GdkVisual *visual=gdk_screen_get_rgba_visual(gtk_widget_get_screen(ui.window));
  if (visual) gtk_widget_set_visual(ui.window,visual);
  gtk_layer_init_for_window(GTK_WINDOW(ui.window));
  gtk_layer_set_layer(GTK_WINDOW(ui.window),GTK_LAYER_SHELL_LAYER_OVERLAY);
  gtk_layer_set_anchor(GTK_WINDOW(ui.window),GTK_LAYER_SHELL_EDGE_LEFT,TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(ui.window),GTK_LAYER_SHELL_EDGE_RIGHT,TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(ui.window),GTK_LAYER_SHELL_EDGE_TOP,TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(ui.window),GTK_LAYER_SHELL_EDGE_BOTTOM,TRUE);
  gtk_layer_set_exclusive_zone(GTK_WINDOW(ui.window),0);
  gtk_layer_set_keyboard_mode(GTK_WINDOW(ui.window),GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
  gtk_layer_set_namespace(GTK_WINDOW(ui.window),kind==WPD_GUI_SWITCHER?"wpd-switcher":
    kind==WPD_GUI_CAROUSEL?"wpd-carousel":kind==WPD_GUI_CONCEPT?"wpd-concept":
    "wpd-concept-v2");
  ui.area=gtk_drawing_area_new();
  gtk_widget_set_app_paintable(ui.area,TRUE);
  GtkCssProvider *css=gtk_css_provider_new();
  gtk_css_provider_load_from_data(css,
    "window#wpd-picker, drawingarea#wpd-picker-area { background-color: transparent; }",-1,NULL);
  gtk_widget_set_name(ui.window,"wpd-picker");
  gtk_widget_set_name(ui.area,"wpd-picker-area");
  gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(ui.window),
    GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
  gtk_container_add(GTK_CONTAINER(ui.window),ui.area);
  gtk_widget_add_events(ui.area,GDK_SCROLL_MASK|GDK_BUTTON_PRESS_MASK);
  g_signal_connect(ui.area,"draw",G_CALLBACK(draw),&ui);
  g_signal_connect(ui.area,"scroll-event",G_CALLBACK(scroll),&ui);
  g_signal_connect(ui.area,"button-press-event",G_CALLBACK(click),&ui);
  g_signal_connect(ui.window,"key-press-event",G_CALLBACK(key_press),&ui);
  g_signal_connect(ui.window,"destroy",G_CALLBACK(gtk_main_quit),NULL);
  gtk_widget_show_all(ui.window);
  for (guint i=0;i<ui.items->len;i++) request_thumb(&ui,g_ptr_array_index(ui.items,i));
  gtk_main();
  if (ui.animation_timer) g_source_remove(ui.animation_timer);
  g_object_set_data(G_OBJECT(ui.area),"wpd-ui",NULL);
  gtk_widget_destroy(ui.window); g_ptr_array_free(ui.items,TRUE);
  return 0;
}
