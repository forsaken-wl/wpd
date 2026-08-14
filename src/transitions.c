#include "transitions.h"
#include <glib.h>
#include <math.h>

static const char *names[WPD_TRANS_COUNT]={
  "fade","wipe","slide","grow","center","corner","infection",
  "radial","diamond","blinds","checker","wave","curtain","clock",
  "dissolve","sweep","curve-corner"
};

static gint base_index(const gchar *name) {
  for (gint i=0;i<WPD_TRANS_COUNT;i++) if (!g_strcmp0(name,names[i])) return i;
  return -1;
}

static gboolean parse_spec(const char *spec,guint *result) {
  if (!spec || !*spec) return FALSE;
  gchar **parts=g_strsplit(spec,"+",-1);
  gint base=base_index(parts[0]);
  if (base<0) { g_strfreev(parts); return FALSE; }
  guint flags=0;
  for (guint i=1;parts[i];i++) {
    const gchar *part=parts[i];
    if (!*part) { g_strfreev(parts); return FALSE; }
    if (!g_strcmp0(part,"invert")) flags|=WPD_TRANS_MOD_INVERT;
    else if (!g_strcmp0(part,"left")) flags&=~(WPD_TRANS_MOD_RIGHT|WPD_TRANS_MOD_VERTICAL);
    else if (!g_strcmp0(part,"right")) { flags|=WPD_TRANS_MOD_RIGHT; flags&=~WPD_TRANS_MOD_VERTICAL; }
    else if (!g_strcmp0(part,"top")) flags&=~(WPD_TRANS_MOD_BOTTOM|WPD_TRANS_MOD_VERTICAL),flags|=WPD_TRANS_MOD_VERTICAL;
    else if (!g_strcmp0(part,"bottom") || !g_strcmp0(part,"btm")) flags|=WPD_TRANS_MOD_BOTTOM|WPD_TRANS_MOD_VERTICAL;
    else if (!g_strcmp0(part,"topleft") || !g_strcmp0(part,"top-left") || !g_strcmp0(part,"tl")) flags&=~(WPD_TRANS_MOD_RIGHT|WPD_TRANS_MOD_BOTTOM|WPD_TRANS_MOD_VERTICAL);
    else if (!g_strcmp0(part,"topright") || !g_strcmp0(part,"top-right") || !g_strcmp0(part,"tr")) { flags|=WPD_TRANS_MOD_RIGHT; flags&=~(WPD_TRANS_MOD_BOTTOM|WPD_TRANS_MOD_VERTICAL); }
    else if (!g_strcmp0(part,"btmleft") || !g_strcmp0(part,"bottomleft") || !g_strcmp0(part,"bottom-left") || !g_strcmp0(part,"bl")) { flags|=WPD_TRANS_MOD_BOTTOM; flags&=~(WPD_TRANS_MOD_RIGHT|WPD_TRANS_MOD_VERTICAL); }
    else if (!g_strcmp0(part,"btmright") || !g_strcmp0(part,"bottomright") || !g_strcmp0(part,"bottom-right") || !g_strcmp0(part,"br")) { flags|=WPD_TRANS_MOD_RIGHT|WPD_TRANS_MOD_BOTTOM; flags&=~WPD_TRANS_MOD_VERTICAL; }
    else { g_strfreev(parts); return FALSE; }
  }
  g_strfreev(parts); *result=(guint)base|flags; return TRUE;
}

gboolean wpd_transition_valid(const char *name) { guint value; return parse_spec(name,&value); }
WpdTransition wpd_transition_parse(const char *name) { guint value; return parse_spec(name,&value)?(WpdTransition)value:WPD_TRANS_FADE; }
const char *wpd_transition_name(WpdTransition transition) { guint base=((guint)transition)&WPD_TRANS_BASE_MASK; return base<WPD_TRANS_COUNT?names[base]:names[0]; }
const char *const *wpd_transition_names(gsize *count) { if(count)*count=WPD_TRANS_COUNT; return names; }

static void paint(cairo_t *cr,cairo_surface_t *surface) {
  if (surface) cairo_set_source_surface(cr,surface,0,0); else cairo_set_source_rgb(cr,0,0,0);
  cairo_paint(cr);
}
static double noise(double x,double y) { return fabs(fmod(sin(x*12.9898+y*78.233)*43758.5453,1.0)); }
static void corner_origin(guint flags,int width,int height,double *x,double *y) {
  *x=(flags&WPD_TRANS_MOD_RIGHT)?width:0; *y=(flags&WPD_TRANS_MOD_BOTTOM)?height:0;
}

void wpd_transition_draw(cairo_t *cr,cairo_surface_t *old,cairo_surface_t *next,
                         WpdTransition encoded,double progress,int width,int height) {
  guint value=(guint)encoded, base=value&WPD_TRANS_BASE_MASK, flags=value&~WPD_TRANS_BASE_MASK;
  double p=CLAMP(progress,0,1); gboolean invert=flags&WPD_TRANS_MOD_INVERT;
  if (invert && base!=WPD_TRANS_CHECKER && base!=WPD_TRANS_DISSOLVE &&
      base!=WPD_TRANS_INFECTION) flags^=WPD_TRANS_MOD_RIGHT|WPD_TRANS_MOD_BOTTOM;
  paint(cr,old); if(p>=1){paint(cr,next);return;} cairo_save(cr);
  if (base==WPD_TRANS_FADE) { cairo_set_source_surface(cr,next,0,0);cairo_paint_with_alpha(cr,p);cairo_restore(cr);return; }
  if (base==WPD_TRANS_WIPE) {
    if (flags&WPD_TRANS_MOD_VERTICAL) cairo_rectangle(cr,0,(flags&WPD_TRANS_MOD_BOTTOM)?height*(1-p):0,width,height*p);
    else cairo_rectangle(cr,(flags&WPD_TRANS_MOD_RIGHT)?width*(1-p):0,0,width*p,height);
  } else if (base==WPD_TRANS_SLIDE) {
    if (flags&WPD_TRANS_MOD_VERTICAL) {
      double y=(flags&WPD_TRANS_MOD_BOTTOM)?height*(1-p):0; cairo_rectangle(cr,0,y,width,height*p);cairo_clip(cr);
      cairo_set_source_surface(cr,next,0,(flags&WPD_TRANS_MOD_BOTTOM)?height*(1-p):-height*(1-p));
    } else {
      double x=(flags&WPD_TRANS_MOD_RIGHT)?width*(1-p):0; cairo_rectangle(cr,x,0,width*p,height);cairo_clip(cr);
      cairo_set_source_surface(cr,next,(flags&WPD_TRANS_MOD_RIGHT)?width*(1-p):-width*(1-p),0);
    }
    cairo_paint(cr);cairo_restore(cr);return;
  } else if (base==WPD_TRANS_GROW) {
    cairo_translate(cr,width/2.0,height/2.0);cairo_scale(cr,p,p);cairo_translate(cr,-width/2.0,-height/2.0);cairo_rectangle(cr,0,0,width,height);
  } else if (base==WPD_TRANS_CENTER) cairo_rectangle(cr,width*(1-p)/2,0,width*p,height);
  else if (base==WPD_TRANS_CORNER) { double x,y;corner_origin(flags,width,height,&x,&y);cairo_arc(cr,x,y,hypot(width,height)*p,0,2*G_PI); }
  else if (base==WPD_TRANS_CURVE_CORNER) {
    double x,y;corner_origin(flags,width,height,&x,&y);cairo_save(cr);cairo_translate(cr,x,y);
    cairo_scale(cr,width,height);cairo_arc(cr,0,0,p*1.45,0,2*G_PI);cairo_restore(cr);
  } else if (base==WPD_TRANS_RADIAL) cairo_arc(cr,width/2.0,height/2.0,hypot(width,height)*p/2,0,2*G_PI);
  else if (base==WPD_TRANS_DIAMOND) {
    double r=(width+height)*p/2;cairo_move_to(cr,width/2.0,height/2.0-r);cairo_line_to(cr,width/2.0+r,height/2.0);
    cairo_line_to(cr,width/2.0,height/2.0+r);cairo_line_to(cr,width/2.0-r,height/2.0);cairo_close_path(cr);
  } else if (base==WPD_TRANS_BLINDS) {
    double band=MAX(32,height/10.0);for(double y=0;y<height;y+=band)
      cairo_rectangle(cr,(flags&WPD_TRANS_MOD_RIGHT)?width*(1-p):0,y,width*p,band+1);
  } else if (base==WPD_TRANS_CHECKER) {
    double cell=MAX(44,MIN(width,height)/11.0);for(double y=0;y<height;y+=cell)for(double x=0;x<width;x+=cell){
      gboolean parity=((gint)(x/cell)+(gint)(y/cell))&1;if(invert)parity=!parity;
      if(p>(parity?.52:.18))cairo_rectangle(cr,x,y,cell+1,cell+1);}
  } else if (base==WPD_TRANS_WAVE) {
    double column=MAX(16,width/64.0);for(double x=0;x<width;x+=column){double edge=height*(p+.12*sin(x/width*6*G_PI));
      cairo_rectangle(cr,x,(flags&WPD_TRANS_MOD_BOTTOM)?height-CLAMP(edge,0,height):0,column+1,CLAMP(edge,0,height));}
  } else if (base==WPD_TRANS_CURTAIN) {
    cairo_rectangle(cr,0,0,width*p/2,height);cairo_rectangle(cr,width-width*p/2,0,width*p/2,height);
  } else if (base==WPD_TRANS_CLOCK) {
    double direction=(flags&WPD_TRANS_MOD_RIGHT)?-1:1;cairo_move_to(cr,width/2.0,height/2.0);
    cairo_arc(cr,width/2.0,height/2.0,hypot(width,height),-G_PI/2,-G_PI/2+direction*2*G_PI*p);cairo_close_path(cr);
  } else if (base==WPD_TRANS_SWEEP) {
    double edge=(width+height)*p;cairo_move_to(cr,0,0);cairo_line_to(cr,MIN(width,edge),0);
    cairo_line_to(cr,MAX(0,edge-height),height);cairo_line_to(cr,0,height);cairo_close_path(cr);
  } else {
    double cell=MAX(42,MIN(width,height)/17.0);for(double y=0;y<height;y+=cell)for(double x=0;x<width;x+=cell){
      gboolean reveal;if(base==WPD_TRANS_INFECTION){double d=hypot(x-width*.5,y-height*.5)/hypot(width*.5,height*.5);
        reveal=d*.72+noise(x,y)*.28<p;if(invert)reveal=(1-d)*.72+noise(x,y)*.28<p;
        if(reveal)cairo_arc(cr,x+cell/2,y+cell/2,cell*(.55+p),0,2*G_PI);
      }else{double n=noise(x,y);reveal=(invert?1-n:n)<p;if(reveal)cairo_rectangle(cr,x,y,cell+1,cell+1);}}
  }
  cairo_clip(cr);paint(cr,next);cairo_restore(cr);
}
