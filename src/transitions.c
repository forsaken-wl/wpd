#include "transitions.h"
#include <glib.h>
#include <math.h>

static const char *names[WPD_TRANS_COUNT]={
  "fade","wipe","slide","grow","center","corner","infection",
  "radial","diamond","blinds","checker","wave","curtain","clock",
  "dissolve","sweep"
};

gboolean wpd_transition_valid(const char *name) {
  for (gint i=0;i<WPD_TRANS_COUNT;i++) if (!g_strcmp0(name,names[i])) return TRUE;
  return FALSE;
}

WpdTransition wpd_transition_parse(const char *name) {
  for (gint i=0;i<WPD_TRANS_COUNT;i++) if (!g_strcmp0(name,names[i])) return i;
  return WPD_TRANS_FADE;
}

const char *wpd_transition_name(WpdTransition transition) {
  return transition>=0 && transition<WPD_TRANS_COUNT ? names[transition] : names[0];
}

const char *const *wpd_transition_names(gsize *count) {
  if (count) *count=WPD_TRANS_COUNT;
  return names;
}

static void paint(cairo_t *cr,cairo_surface_t *surface) {
  if (surface) cairo_set_source_surface(cr,surface,0,0);
  else cairo_set_source_rgb(cr,0,0,0);
  cairo_paint(cr);
}

static double noise(double x,double y) {
  return fabs(fmod(sin(x*12.9898+y*78.233)*43758.5453,1.0));
}

void wpd_transition_draw(cairo_t *cr,cairo_surface_t *old,cairo_surface_t *next,
                         WpdTransition transition,double progress,int width,int height) {
  double p=CLAMP(progress,0,1);
  paint(cr,old);
  if (p>=1) { paint(cr,next); return; }
  cairo_save(cr);
  if (transition==WPD_TRANS_FADE) {
    cairo_set_source_surface(cr,next,0,0); cairo_paint_with_alpha(cr,p);
    cairo_restore(cr); return;
  }
  if (transition==WPD_TRANS_WIPE) cairo_rectangle(cr,0,0,width*p,height);
  else if (transition==WPD_TRANS_SLIDE) {
    cairo_rectangle(cr,0,0,width*p,height); cairo_clip(cr);
    cairo_set_source_surface(cr,next,-width*(1-p),0); cairo_paint(cr);
    cairo_restore(cr); return;
  } else if (transition==WPD_TRANS_GROW) {
    cairo_translate(cr,width/2.0,height/2.0); cairo_scale(cr,p,p);
    cairo_translate(cr,-width/2.0,-height/2.0); cairo_rectangle(cr,0,0,width,height);
  } else if (transition==WPD_TRANS_CENTER)
    cairo_rectangle(cr,width*(1-p)/2,0,width*p,height);
  else if (transition==WPD_TRANS_CORNER)
    cairo_arc(cr,0,0,hypot(width,height)*p,0,2*G_PI);
  else if (transition==WPD_TRANS_RADIAL)
    cairo_arc(cr,width/2.0,height/2.0,hypot(width,height)*p/2,0,2*G_PI);
  else if (transition==WPD_TRANS_DIAMOND) {
    double radius=(width+height)*p/2;
    cairo_move_to(cr,width/2.0,height/2.0-radius);
    cairo_line_to(cr,width/2.0+radius,height/2.0);
    cairo_line_to(cr,width/2.0,height/2.0+radius);
    cairo_line_to(cr,width/2.0-radius,height/2.0); cairo_close_path(cr);
  } else if (transition==WPD_TRANS_BLINDS) {
    double band=MAX(24,height/12.0);
    for (double y=0;y<height;y+=band) cairo_rectangle(cr,0,y,width*p,band+1);
  } else if (transition==WPD_TRANS_CHECKER) {
    double cell=MAX(32,MIN(width,height)/14.0);
    for (double y=0;y<height;y+=cell) for (double x=0;x<width;x+=cell)
      if (fmod(x/cell+y/cell,2)<1 ? p>.25 : p>.55)
        cairo_rectangle(cr,x,y,cell+1,cell+1);
  } else if (transition==WPD_TRANS_WAVE) {
    double column=MAX(8,width/100.0);
    for (double x=0;x<width;x+=column) {
      double edge=height*(p+.12*sin(x/width*6*G_PI));
      cairo_rectangle(cr,x,0,column+1,CLAMP(edge,0,height));
    }
  } else if (transition==WPD_TRANS_CURTAIN) {
    cairo_rectangle(cr,0,0,width*p/2,height);
    cairo_rectangle(cr,width-width*p/2,0,width*p/2,height);
  } else if (transition==WPD_TRANS_CLOCK) {
    cairo_move_to(cr,width/2.0,height/2.0);
    cairo_arc(cr,width/2.0,height/2.0,hypot(width,height),-G_PI/2,
              -G_PI/2+2*G_PI*p); cairo_close_path(cr);
  } else if (transition==WPD_TRANS_SWEEP) {
    double edge=(width+height)*p;
    cairo_move_to(cr,0,0); cairo_line_to(cr,MIN(width,edge),0);
    cairo_line_to(cr,MAX(0,edge-height),height); cairo_line_to(cr,0,height);
    cairo_close_path(cr);
  } else {
    double cell=MAX(24,MIN(width,height)/24.0);
    for (double y=0;y<height;y+=cell) for (double x=0;x<width;x+=cell) {
      gboolean reveal;
      if (transition==WPD_TRANS_INFECTION) {
        double distance=hypot(x-width*.5,y-height*.5)/hypot(width*.5,height*.5);
        reveal=distance*.72+noise(x,y)*.28<p;
        if (reveal) cairo_arc(cr,x+cell/2,y+cell/2,cell*(.55+p),0,2*G_PI);
      } else {
        reveal=noise(x,y)<p;
        if (reveal) cairo_rectangle(cr,x,y,cell+1,cell+1);
      }
    }
  }
  cairo_clip(cr); paint(cr,next); cairo_restore(cr);
}
