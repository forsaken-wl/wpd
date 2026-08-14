#include "common.h"
#include "../image.h"
#include "../ipc.h"
#include "../wallpaper.h"
#include "../collection.h"
#include <gtk-layer-shell.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { gint refs; gchar *path; cairo_surface_t *thumb; gboolean directory; } Item;
typedef struct {
  WpdConfig *config;
  WpdGuiKind kind;
  GtkWidget *window, *area;
  GPtrArray *items;
  gint selected;
  gint hovered;
  SwitcherConfig style;
  guint animation_timer;
  gint64 animation_last_frame;
  double animation_offset, animation_velocity;
  gchar *browse_dir;
  gchar *status;
} Ui;
typedef struct { gchar *path, *cache; Item *item; } ThumbJob;
typedef struct { gint index; double position; } RenderEntry;

static gboolean browses_collections(WpdGuiKind kind) {
  return kind==WPD_GUI_SWITCHER || kind==WPD_GUI_GRID ||
         kind==WPD_GUI_FILMSTRIP || kind==WPD_GUI_ROOTS;
}

static void quit_if_running(GtkWidget *widget,gpointer data) {
  (void)widget;(void)data;
  if (gtk_main_level()>0) gtk_main_quit();
}

static gint render_entry_compare(gconstpointer a, gconstpointer b) {
  const RenderEntry *left=a, *right=b;
  double difference=fabs(right->position)-fabs(left->position);
  return difference<0 ? -1 : difference>0 ? 1 : 0;
}

static Item *item_ref(Item *item) {
  g_atomic_int_inc(&item->refs);
  return item;
}

static gint item_compare(gconstpointer left,gconstpointer right) {
  const Item *a=*(Item *const *)left,*b=*(Item *const *)right;
  if (a->directory!=b->directory) return a->directory?-1:1;
  gchar *an=g_path_get_basename(a->path),*bn=g_path_get_basename(b->path);
  gint result=g_utf8_collate(an,bn);g_free(an);g_free(bn);return result;
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

static void load_collection_directory(Ui *ui,const gchar *directory) {
  /* The caller may pass a path owned by the currently selected item. Keep our
     own copy before clearing the array that owns that item. */
  gchar *target=g_strdup(directory);
  if (ui->animation_timer) {g_source_remove(ui->animation_timer);ui->animation_timer=0;}
  ui->animation_offset=ui->animation_velocity=0;ui->selected=0;ui->hovered=-1;
  g_ptr_array_set_size(ui->items,0);
  GDir *dir=g_dir_open(target,0,NULL);
  if (dir) {
    const gchar *name;
    while ((name=g_dir_read_name(dir))) {
      gchar *path=g_build_filename(target,name,NULL);
      gboolean is_dir=g_file_test(path,G_FILE_TEST_IS_DIR)&&
                       !g_file_test(path,G_FILE_TEST_IS_SYMLINK);
      if (is_dir || (g_file_test(path,G_FILE_TEST_IS_REGULAR)&&
                     wpd_image_extension_supported(name))) {
        Item *item=g_new0(Item,1);item->refs=1;item->path=path;item->directory=is_dir;
        g_ptr_array_add(ui->items,item);
      } else g_free(path);
    }
    g_dir_close(dir);
  }
  g_ptr_array_sort(ui->items,item_compare);
  g_free(ui->browse_dir);ui->browse_dir=target;
  for (guint i=0;i<ui->items->len;i++) {
    Item *item=g_ptr_array_index(ui->items,i);
    if (!item->directory) request_thumb(ui,item);
  }
  gtk_widget_queue_draw(ui->area);
}

static void draw_status(cairo_t *cr,Ui *ui,gint width,gint height) {
  if (!ui->status) return;
  GdkRGBA accent=ui->style.selected_color;
  cairo_set_source_rgba(cr,accent.red,accent.green,accent.blue,accent.alpha);
  cairo_set_font_size(cr,14);cairo_text_extents_t extents;
  cairo_text_extents(cr,ui->status,&extents);
  cairo_move_to(cr,width/2-extents.width/2-extents.x_bearing,height-28);
  cairo_show_text(cr,ui->status);
}

static void apply_selected(Ui *ui) {
  if (!ui->items->len) return;
  Item *item = g_ptr_array_index(ui->items, ui->selected);
  if (browses_collections(ui->kind) && item->directory) {
    g_clear_pointer(&ui->status,g_free);load_collection_directory(ui,item->path);return;
  }
  GError *theme_error=NULL;
  wpd_collection_apply_theme_for_path(ui->config,item->path,&theme_error);
  if (theme_error) {g_printerr("wpd: %s\n",theme_error->message);g_clear_error(&theme_error);}
  gchar *request = g_strdup_printf("IMAGE\tfill\t%s", item->path);
  gchar *reply = NULL; GError *error = NULL;
  if (!wpd_ipc_send(ui->config->socket_path, request, &reply, &error)) {
    g_printerr("wpd: %s\n", error->message);
    g_free(ui->status);ui->status=g_strdup("Daemon unavailable — run: wpd daemon");
    gtk_widget_queue_draw(ui->area);
  } else if (!g_str_has_prefix(reply, "OK")) {
    g_printerr("wpd: %s\n", reply);
    g_free(ui->status);ui->status=g_strdup(reply);gtk_widget_queue_draw(ui->area);
  } else
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
  if (ui->kind==WPD_GUI_GRID) { gtk_widget_queue_draw(ui->area); return; }
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
  GtkAllocation area;gtk_widget_get_allocation(ui->area,&area);
  gint columns=CLAMP(area.width/(ui->style.side_width+34),2,6);
  if (event->keyval == GDK_KEY_Left || event->keyval==GDK_KEY_h) select_delta(ui,-1);
  else if (event->keyval == GDK_KEY_Right || event->keyval==GDK_KEY_l) select_delta(ui,1);
  else if (ui->kind==WPD_GUI_GRID &&
           (event->keyval==GDK_KEY_Up || event->keyval==GDK_KEY_k))
    select_delta(ui,-columns);
  else if (ui->kind==WPD_GUI_GRID &&
           (event->keyval==GDK_KEY_Down || event->keyval==GDK_KEY_j))
    select_delta(ui,columns);
  else if (event->keyval == GDK_KEY_j) select_delta(ui,1);
  else if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
    apply_selected(ui);
  else if (browses_collections(ui->kind) && event->keyval==GDK_KEY_BackSpace) {
    if (g_strcmp0(ui->browse_dir,ui->config->papers_dir)) {
      gchar *parent=g_path_get_dirname(ui->browse_dir);
      load_collection_directory(ui,parent);g_free(parent);
    }
  }
  else if (browses_collections(ui->kind) && event->keyval==GDK_KEY_Home)
    load_collection_directory(ui,ui->config->papers_dir);
  else if (event->keyval == GDK_KEY_Escape) gtk_main_quit();
  else return FALSE;
  return TRUE;
}

static gboolean scroll(GtkWidget *widget, GdkEventScroll *event, gpointer data) {
  (void)widget;
  Ui *ui=data;
  gint delta=event->direction==GDK_SCROLL_UP || event->delta_y<0 ? -1 : 1;
  select_delta(ui,delta);
  return TRUE;
}

static gboolean pointer_motion(GtkWidget *widget,GdkEventMotion *event,gpointer data) {
  Ui *ui=data;
  if (ui->kind!=WPD_GUI_SWITCHER || !ui->items->len) return FALSE;
  GtkAllocation area;gtk_widget_get_allocation(widget,&area);
  double natural=4*ui->style.spacing+ui->style.side_width+76;
  double scale=CLAMP((area.width-48.0)/natural,.35,1.0);
  double spacing=ui->style.spacing*scale;
  gint distance=(gint)round((event->x-area.width/2-ui->animation_offset*scale)/spacing);
  gint previous=ui->hovered;
  ui->hovered=distance>=-2&&distance<=2?
    (ui->selected+distance+(gint)ui->items->len)%(gint)ui->items->len:-1;
  if (previous!=ui->hovered) gtk_widget_queue_draw(ui->area);
  return TRUE;
}

static gboolean pointer_leave(GtkWidget *widget,GdkEventCrossing *event,gpointer data) {
  (void)widget;(void)event;Ui *ui=data;
  if (ui->hovered>=0){ui->hovered=-1;gtk_widget_queue_draw(ui->area);}
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
  if (item->directory) {
    GdkRGBA background=ui->style.background_color;
    cairo_set_source_rgba(cr,background.red,background.green,background.blue,1);
    cairo_paint(cr);
    /* Lucide folder geometry, scaled from its 24x24 view box. */
    double size=MIN(w,h)*.48,unit=size/24.0,fx=x+(w-size)/2,fy=y+h*.20;
    cairo_move_to(cr,fx+20*unit,fy+20*unit);
    cairo_curve_to(cr,fx+21.1*unit,fy+20*unit,fx+22*unit,fy+19.1*unit,
                   fx+22*unit,fy+18*unit);
    cairo_line_to(cr,fx+22*unit,fy+8*unit);
    cairo_curve_to(cr,fx+22*unit,fy+6.9*unit,fx+21.1*unit,fy+6*unit,
                   fx+20*unit,fy+6*unit);
    cairo_line_to(cr,fx+12.1*unit,fy+6*unit);
    cairo_curve_to(cr,fx+11.4*unit,fy+6*unit,fx+10.75*unit,fy+5.65*unit,
                   fx+10.4*unit,fy+5.1*unit);
    cairo_line_to(cr,fx+9.6*unit,fy+3.9*unit);
    cairo_curve_to(cr,fx+9.23*unit,fy+3.34*unit,fx+8.61*unit,fy+3*unit,
                   fx+7.93*unit,fy+3*unit);
    cairo_line_to(cr,fx+4*unit,fy+3*unit);
    cairo_curve_to(cr,fx+2.9*unit,fy+3*unit,fx+2*unit,fy+3.9*unit,
                   fx+2*unit,fy+5*unit);
    cairo_line_to(cr,fx+2*unit,fy+18*unit);
    cairo_curve_to(cr,fx+2*unit,fy+19.1*unit,fx+2.9*unit,fy+20*unit,
                   fx+4*unit,fy+20*unit);
    cairo_close_path(cr);
    GdkRGBA icon=selected?ui->style.selected_color:ui->style.foreground_color;
    cairo_set_source_rgba(cr,icon.red,icon.green,icon.blue,selected?1:.82);
    cairo_set_line_width(cr,2*unit);cairo_set_line_join(cr,CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);cairo_stroke(cr);
    gchar *name=g_path_get_basename(item->path);cairo_text_extents_t extents;
    cairo_set_font_size(cr,MIN(16.0,w/12));cairo_text_extents(cr,name,&extents);
    GdkRGBA foreground=ui->style.foreground_color;
    cairo_set_source_rgba(cr,foreground.red,foreground.green,foreground.blue,
                          foreground.alpha);
    cairo_move_to(cr,x+w/2-extents.width/2-extents.x_bearing,y+h*.82);
    cairo_show_text(cr,name);g_free(name);
  } else if (item->thumb) {
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
  if (item->thumb) {
    double sw=cairo_image_surface_get_width(item->thumb);
    double sh=cairo_image_surface_get_height(item->thumb);
    double crop_scale=MAX(w/sw,h/sh);
    double crop_w=w/crop_scale, crop_h=h/crop_scale;
    double source_x=(sw-crop_w)/2, source_y=(sh-crop_h)/2;
    double inset=MIN(h*.28,fabs(tilt)*h*.22);
    const gint strips=48;
    for (gint strip=0;strip<strips;strip++) {
      double t0=(double)strip/strips, t1=(double)(strip+1)/strips;
      double top0=tilt<0?inset*(1-t0):inset*t0;
      double top1=tilt<0?inset*(1-t1):inset*t1;
      double bottom0=h-top0, bottom1=h-top1;
      double x0=x+w*t0, x1=x+w*t1;
      cairo_save(cr);
      cairo_set_antialias(cr,CAIRO_ANTIALIAS_NONE);
      cairo_move_to(cr,x0,y+top0);
      cairo_line_to(cr,x1,y+top1); cairo_line_to(cr,x1,y+bottom1);
      cairo_line_to(cr,x0,y+bottom0); cairo_close_path(cr); cairo_clip(cr);
      double middle=(t0+t1)/2;
      double top_middle=tilt<0?inset*(1-middle):inset*middle;
      double strip_height=h-2*top_middle;
      double sx0=source_x+crop_w*t0, sx1=source_x+crop_w*t1;
      cairo_translate(cr,x0,y+top_middle);
      cairo_scale(cr,(x1-x0)/(sx1-sx0),strip_height/crop_h);
      cairo_set_source_surface(cr,item->thumb,-sx0,-source_y);
      cairo_pattern_set_filter(cairo_get_source(cr),CAIRO_FILTER_BILINEAR);
      cairo_pattern_set_extend(cairo_get_source(cr),CAIRO_EXTEND_PAD);
      cairo_paint(cr); cairo_restore(cr);
    }
  } else {
    perspective_path(cr,x,y,w,h,tilt); cairo_save(cr); cairo_clip(cr);
    GdkRGBA background=ui->style.background_color;
    cairo_set_source_rgba(cr,background.red,background.green,background.blue,1);
    cairo_paint(cr); cairo_restore(cr);
  }
  if (!selected && ui->style.dim>0) {
    perspective_path(cr,x,y,w,h,tilt);
    cairo_set_source_rgba(cr,0,0,0,ui->style.dim/100.0); cairo_fill(cr);
  }
  perspective_path(cr,x,y,w,h,tilt); cairo_set_line_width(cr,ui->style.border);
  cairo_set_line_join(cr,CAIRO_LINE_JOIN_ROUND);
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
    cairo_show_text(cr,browses_collections(ui->kind)?
      "This collection is empty — Backspace returns to its parent":
      "No wallpapers in the WPD papers directory");draw_status(cr,ui,area.width,area.height);
    return FALSE;
  }
  if (browses_collections(ui->kind) && ui->kind!=WPD_GUI_SWITCHER) {
    const gchar *relative=ui->browse_dir+strlen(ui->config->papers_dir);
    while (*relative==G_DIR_SEPARATOR) relative++;
    gchar *crumb=*relative?g_strdup_printf("Papers  /  %s",relative):g_strdup("Papers");
    GdkRGBA foreground=ui->style.foreground_color;
    cairo_set_source_rgba(cr,foreground.red,foreground.green,foreground.blue,
                          foreground.alpha*.82);
    cairo_set_font_size(cr,12);cairo_move_to(cr,32,40);cairo_show_text(cr,crumb);
    g_free(crumb);
  }
  if (ui->kind==WPD_GUI_GRID) {
    gint columns=CLAMP(area.width/(ui->style.side_width+34),2,6);
    double gap=18, card_w=MIN((area.width-80-(columns-1)*gap)/columns,
                              (double)ui->style.selected_width);
    double card_h=card_w*ui->style.height/ui->style.side_width;
    gint rows=MAX(1,(gint)((area.height-110)/(card_h+gap)));
    gint per_page=columns*rows, page=ui->selected/per_page;
    gint first=page*per_page, last=MIN(first+per_page,(gint)ui->items->len);
    double panel_w=columns*card_w+(columns-1)*gap+36;
    double panel_h=rows*card_h+(rows-1)*gap+70;
    double panel_x=(area.width-panel_w)/2,panel_y=(area.height-panel_h)/2;
    if (ui->style.band) {
      GdkRGBA bg=ui->style.background_color;
      cairo_rectangle(cr,panel_x,panel_y,panel_w,panel_h);
      cairo_set_source_rgba(cr,bg.red,bg.green,bg.blue,bg.alpha);cairo_fill(cr);
    }
    for (gint index=first;index<last;index++) {
      gint cell=index-first,row=cell/columns,column=cell%columns;
      double x=panel_x+18+column*(card_w+gap);
      double y=panel_y+18+row*(card_h+gap);
      paint_card(cr,g_ptr_array_index(ui->items,index),x,y,card_w,card_h,ui,
                 index==ui->selected);
    }
    Item *chosen=g_ptr_array_index(ui->items,ui->selected);
    gchar *name=g_path_get_basename(chosen->path);
    gchar *page_text=g_strdup_printf("%s   ·   %d / %u   ·   page %d / %d",name,
      ui->selected+1,ui->items->len,page+1,
      ((gint)ui->items->len+per_page-1)/per_page);
    GdkRGBA fg=ui->style.foreground_color;
    cairo_set_source_rgba(cr,fg.red,fg.green,fg.blue,fg.alpha);
    cairo_set_font_size(cr,13);cairo_move_to(cr,panel_x+18,panel_y+panel_h-16);
    cairo_show_text(cr,page_text);g_free(page_text);g_free(name);
    draw_status(cr,ui,area.width,area.height);return FALSE;
  }
  if (ui->kind==WPD_GUI_3D) {
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
  if (ui->kind==WPD_GUI_STACK) {
    gint count=(gint)ui->items->len;
    double offset=ui->animation_offset/ui->style.spacing;
    RenderEntry *entries=g_new(RenderEntry,count);
    for (gint i=0;i<count;i++) {
      double p=i-ui->selected;
      while(p>count/2.0)p-=count;
      while(p< -count/2.0)p+=count;
      entries[i]=(RenderEntry){i,p+offset};
    }
    qsort(entries,count,sizeof(RenderEntry),render_entry_compare);
    for(gint slot=0;slot<count;slot++) {
      double p=entries[slot].position;if(fabs(p)>4.2)continue;
      double focus=1-MIN(1.0,fabs(p)),scale=.72+.28*focus;
      double w=ui->style.selected_width*scale,h=ui->style.height*scale;
      double x=area.width/2-w/2+p*72,y=area.height/2-h/2+fabs(p)*17;
      paint_card(cr,g_ptr_array_index(ui->items,entries[slot].index),x,y,w,h,ui,
                 focus>.5);
    }
    Item *chosen=g_ptr_array_index(ui->items,ui->selected);
    gchar *name=g_path_get_basename(chosen->path);GdkRGBA fg=ui->style.foreground_color;
    cairo_set_source_rgba(cr,fg.red,fg.green,fg.blue,fg.alpha);cairo_set_font_size(cr,16);
    cairo_move_to(cr,area.width/2-strlen(name)*4.0,
                  area.height/2+ui->style.height/2+55);cairo_show_text(cr,name);
    g_free(name);g_free(entries);return FALSE;
  }
  if (ui->kind==WPD_GUI_ROOTS) {
    gint count=(gint)ui->items->len;
    double card_w=MAX(92.0,MIN((double)ui->style.side_width,
                              (area.width-204.0)/7.0));
    double card_h=card_w*ui->style.height/ui->style.side_width;
    double roots_spacing=card_w+12;
    double panel_w=MIN(area.width-48.0,6*roots_spacing+card_w+48);
    double panel_h=card_h+92;
    double panel_x=(area.width-panel_w)/2,panel_y=(area.height-panel_h)/2;
    GdkRGBA bg=ui->style.background_color,accent=ui->style.selected_color;
    cairo_rectangle(cr,panel_x,panel_y,panel_w,panel_h);
    cairo_set_source_rgba(cr,bg.red,bg.green,bg.blue,MIN(bg.alpha,.82));
    cairo_fill(cr);
    cairo_rectangle(cr,panel_x,panel_y,panel_w,4);
    cairo_set_source_rgba(cr,accent.red,accent.green,accent.blue,accent.alpha);
    cairo_fill(cr);
    cairo_save(cr);cairo_rectangle(cr,panel_x+12,panel_y+12,panel_w-24,panel_h-24);
    cairo_clip(cr);
    double offset=ui->animation_offset/ui->style.spacing;
    RenderEntry *entries=g_new(RenderEntry,count);
    for(gint i=0;i<count;i++) {
      double p=i-ui->selected;
      while(p>count/2.0)p-=count;
      while(p< -count/2.0)p+=count;
      entries[i]=(RenderEntry){i,p+offset};
    }
    qsort(entries,count,sizeof(RenderEntry),render_entry_compare);
    gint old_skew=ui->style.skew,old_radius=ui->style.radius;
    ui->style.skew=0;ui->style.radius=2;
    for(gint slot=0;slot<count;slot++) {
      double p=entries[slot].position;if(fabs(p)>3.6)continue;
      gboolean selected=fabs(p)<.5;
      double x=area.width/2+p*roots_spacing-card_w/2;
      double y=panel_y+25+(selected?-7:0);
      paint_card(cr,g_ptr_array_index(ui->items,entries[slot].index),x,y,
                 card_w,card_h,ui,selected);
    }
    ui->style.skew=old_skew;ui->style.radius=old_radius;
    g_free(entries);cairo_restore(cr);
    Item *chosen=g_ptr_array_index(ui->items,ui->selected);
    gchar *name=g_path_get_basename(chosen->path),*status=g_strdup_printf("%d / %u",
      ui->selected+1,ui->items->len);GdkRGBA fg=ui->style.foreground_color;
    cairo_set_source_rgba(cr,fg.red,fg.green,fg.blue,fg.alpha);cairo_set_font_size(cr,15);
    cairo_move_to(cr,panel_x+20,panel_y+panel_h-20);cairo_show_text(cr,name);
    cairo_set_source_rgba(cr,accent.red,accent.green,accent.blue,accent.alpha);
    cairo_move_to(cr,panel_x+panel_w-60,panel_y+panel_h-20);cairo_show_text(cr,status);
    g_free(status);g_free(name);draw_status(cr,ui,area.width,area.height);return FALSE;
  }
  double natural_w=4*ui->style.spacing+ui->style.side_width+76;
  double layout_scale=ui->kind==WPD_GUI_SWITCHER?
    CLAMP((area.width-48.0)/natural_w,.35,1.0):1.0;
  double spacing=ui->style.spacing*layout_scale;
  double deck_w=natural_w*layout_scale;
  double deck_h=(ui->style.height+62)*layout_scale;
  double deck_x = (area.width-deck_w)/2, deck_y = (area.height-deck_h)/2;
  if (ui->kind == WPD_GUI_SWITCHER && ui->style.band) {
    cairo_rectangle(cr,deck_x,deck_y,deck_w,deck_h);
    GdkRGBA background=ui->style.background_color;
    cairo_set_source_rgba(cr,background.red,background.green,background.blue,
                          background.alpha);
    cairo_fill_preserve(cr);
    GdkRGBA accent=ui->style.selected_color;
    cairo_set_source_rgba(cr,accent.red,accent.green,accent.blue,accent.alpha*.7);
    cairo_set_line_width(cr,MAX(1.0,ui->style.border));
    cairo_stroke(cr);
  }
  cairo_save(cr);
  if (ui->kind == WPD_GUI_SWITCHER) {
    cairo_rectangle(cr,deck_x,deck_y,deck_w,deck_h);
    cairo_clip(cr);
    const gchar *relative=ui->browse_dir+strlen(ui->config->papers_dir);
    while (*relative==G_DIR_SEPARATOR) relative++;
    gchar *crumb=*relative?g_strdup_printf("Papers  /  %s",relative):g_strdup("Papers");
    GdkRGBA foreground=ui->style.foreground_color;
    cairo_set_source_rgba(cr,foreground.red,foreground.green,foreground.blue,
                          foreground.alpha*.8);
    cairo_set_font_size(cr,MAX(9.0,11*layout_scale));
    cairo_move_to(cr,deck_x+12*layout_scale,deck_y+17*layout_scale);
    cairo_show_text(cr,crumb);g_free(crumb);
  }
  gint base_range = ui->kind == WPD_GUI_SWITCHER ? 2 :
                    ui->kind==WPD_GUI_FILMSTRIP ? 4 : 3;
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
    double width = (ui->style.side_width +
      (ui->style.selected_width-ui->style.side_width)*focus)*layout_scale;
    double height = ui->style.height*layout_scale;
    if (ui->kind == WPD_GUI_CAROUSEL) {
      double scale = .55 + 1.20*focus;
      width = ui->style.selected_width * scale; height *= scale;
    }
    if (ui->kind==WPD_GUI_FILMSTRIP) {
      width=ui->style.side_width*(selected?1.08:1.0);
      height=ui->style.height*(selected?1.08:.9);
    }
    double x = area.width/2.0 + position*spacing - width/2;
    double y = area.height/2.0 - height/2;
    if (ui->kind==WPD_GUI_SWITCHER && index==ui->hovered && !selected)
      y-=5*layout_scale;
    paint_card(cr, g_ptr_array_index(ui->items,index), x,y,width,height,ui,selected);
    if (selected && (ui->kind == WPD_GUI_CAROUSEL ||
                     ui->kind==WPD_GUI_FILMSTRIP)) {
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
  if (ui->kind==WPD_GUI_SWITCHER) {
    Item *chosen=g_ptr_array_index(ui->items,ui->selected);
    gchar *name=g_path_get_basename(chosen->path);
    GdkRGBA foreground=ui->style.foreground_color;
    GdkRGBA accent=ui->style.selected_color;
    cairo_text_extents_t extents;
    cairo_set_font_size(cr,MAX(10.0,14*layout_scale));
    cairo_text_extents(cr,name,&extents);
    cairo_set_source_rgba(cr,foreground.red,foreground.green,foreground.blue,
                          foreground.alpha);
    double label_y=deck_y+deck_h-12*layout_scale;
    cairo_move_to(cr,area.width/2-extents.width/2-extents.x_bearing,label_y);
    cairo_show_text(cr,name);
    gchar *position=chosen->directory?
      g_strdup_printf("folder  ·  Enter   %d / %u",ui->selected+1,ui->items->len):
      g_strdup_printf("%d / %u",ui->selected+1,ui->items->len);
    cairo_set_font_size(cr,MAX(9.0,11*layout_scale));
    cairo_text_extents(cr,position,&extents);
    cairo_set_source_rgba(cr,accent.red,accent.green,accent.blue,accent.alpha);
    cairo_move_to(cr,deck_x+deck_w-12*layout_scale-extents.width-extents.x_bearing,
                  label_y);
    cairo_show_text(cr,position);
    cairo_set_line_width(cr,2*layout_scale);
    cairo_move_to(cr,area.width/2-28*layout_scale,label_y+6*layout_scale);
    cairo_line_to(cr,area.width/2+28*layout_scale,label_y+6*layout_scale);
    cairo_stroke(cr);
    g_free(position); g_free(name);
  }
  cairo_restore(cr);
  draw_status(cr,ui,area.width,area.height);
  return FALSE;
}

static gboolean click(GtkWidget *widget, GdkEventButton *event, gpointer data) {
  Ui *ui = data; GtkAllocation area; gtk_widget_get_allocation(widget, &area);
  if (ui->kind==WPD_GUI_GRID) {
    gint columns=CLAMP(area.width/(ui->style.side_width+34),2,6);
    double gap=18,card_w=MIN((area.width-80-(columns-1)*gap)/columns,
                             (double)ui->style.selected_width);
    double card_h=card_w*ui->style.height/ui->style.side_width;
    gint rows=MAX(1,(gint)((area.height-110)/(card_h+gap)));
    gint per_page=columns*rows,first=(ui->selected/per_page)*per_page;
    double panel_w=columns*card_w+(columns-1)*gap+36;
    double panel_h=rows*card_h+(rows-1)*gap+70;
    double local_x=event->x-(area.width-panel_w)/2-18;
    double local_y=event->y-(area.height-panel_h)/2-18;
    gint column=(gint)(local_x/(card_w+gap)),row=(gint)(local_y/(card_h+gap));
    gint index=first+row*columns+column;
    if(local_x>=0&&local_y>=0&&column>=0&&column<columns&&row>=0&&row<rows&&
       fmod(local_x,card_w+gap)<=card_w&&fmod(local_y,card_h+gap)<=card_h&&
       index<(gint)ui->items->len) {
      if(index==ui->selected)apply_selected(ui);
      else {ui->selected=index;gtk_widget_queue_draw(ui->area);}
    }
    return TRUE;
  }
  if (ui->kind==WPD_GUI_3D) {
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
  gint range = ui->kind == WPD_GUI_SWITCHER ? 2 :
               ui->kind==WPD_GUI_FILMSTRIP ? 4 : 3, closest = 0;
  double best = G_MAXDOUBLE;
  double natural=4*ui->style.spacing+ui->style.side_width+76;
  double scale=ui->kind==WPD_GUI_SWITCHER?
    CLAMP((area.width-48.0)/natural,.35,1.0):1.0;
  double roots_width=MAX(92.0,MIN((double)ui->style.side_width,
                                  (area.width-204.0)/7.0));
  double spacing=ui->kind==WPD_GUI_STACK?72:
                 ui->kind==WPD_GUI_ROOTS?roots_width+12:ui->style.spacing*scale;
  double animation_scale=ui->kind==WPD_GUI_STACK?72/ui->style.spacing:
                         ui->kind==WPD_GUI_ROOTS?spacing/ui->style.spacing:scale;
  for (gint distance=-range; distance<=range; distance++) {
    double delta = fabs(event->x-(area.width/2.0+distance*spacing+
                                  ui->animation_offset*animation_scale));
    if (delta < best) { best = delta; closest = distance; }
  }
  if (!closest) apply_selected(ui); else select_delta(ui, closest);
  return TRUE;
}

int wpd_gui_run(WpdConfig *config, WpdGuiKind kind) {
  Ui ui = {0}; ui.config=config; ui.kind=kind; ui.hovered=-1;
  wpd_switcher_config_load(config,&ui.style);
  g_mkdir_with_parents(config->cache_dir,0700);
  ui.items = g_ptr_array_new_with_free_func(item_unref);
  if (config->socket_path) {
    gchar *reply=NULL;GError *error=NULL;
    if (!wpd_ipc_send(config->socket_path,"PING",&reply,&error))
      ui.status=g_strdup("Daemon unavailable — run: wpd daemon");
    g_free(reply);g_clear_error(&error);
  } else ui.status=g_strdup("XDG_RUNTIME_DIR is unavailable");
  if (!browses_collections(kind)) {
    GPtrArray *paths=wpd_wallpapers_scan(config->papers_dir);
    for (guint i=0; i<paths->len; i++) {
      Item *item=g_new0(Item,1); item->refs=1;
      item->path=g_strdup(g_ptr_array_index(paths,i));
      g_ptr_array_add(ui.items,item);
    }
    g_ptr_array_free(paths,TRUE);
  }
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
  const gchar *namespaces[]={"wpd-switcher","wpd-carousel","wpd-3d",
                             "wpd-grid","wpd-stack","wpd-filmstrip","wpd-roots"};
  gtk_layer_set_namespace(GTK_WINDOW(ui.window),namespaces[kind]);
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
  gtk_widget_add_events(ui.area,GDK_SCROLL_MASK|GDK_BUTTON_PRESS_MASK|
                        GDK_POINTER_MOTION_MASK|GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(ui.area,"draw",G_CALLBACK(draw),&ui);
  g_signal_connect(ui.area,"scroll-event",G_CALLBACK(scroll),&ui);
  g_signal_connect(ui.area,"button-press-event",G_CALLBACK(click),&ui);
  g_signal_connect(ui.area,"motion-notify-event",G_CALLBACK(pointer_motion),&ui);
  g_signal_connect(ui.area,"leave-notify-event",G_CALLBACK(pointer_leave),&ui);
  g_signal_connect(ui.window,"key-press-event",G_CALLBACK(key_press),&ui);
  g_signal_connect(ui.window,"destroy",G_CALLBACK(quit_if_running),NULL);
  if (browses_collections(kind)) load_collection_directory(&ui,config->papers_dir);
  gtk_widget_show_all(ui.window);
  if (!browses_collections(kind))
    for (guint i=0;i<ui.items->len;i++) request_thumb(&ui,g_ptr_array_index(ui.items,i));
  gtk_main();
  if (ui.animation_timer) g_source_remove(ui.animation_timer);
  g_object_set_data(G_OBJECT(ui.area),"wpd-ui",NULL);
  gtk_widget_destroy(ui.window);g_free(ui.browse_dir);g_free(ui.status);
  g_ptr_array_free(ui.items,TRUE);
  return 0;
}
