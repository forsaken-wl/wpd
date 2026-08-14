#include "transitions.h"
#include <glib.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static const char*names[WPD_TRANS_COUNT]={"fade","wipe","slide","grow","center","corner","infection"};
WpdTransition wpd_transition_parse(const char*n){for(int i=0;i<WPD_TRANS_COUNT;i++)if(!g_strcmp0(n,names[i]))return i;return WPD_TRANS_FADE;}const char*wpd_transition_name(WpdTransition t){return t>=0&&t<WPD_TRANS_COUNT?names[t]:names[0];}
static void paint(cairo_t*c,cairo_surface_t*s){if(s){cairo_set_source_surface(c,s,0,0);cairo_paint(c);}else{cairo_set_source_rgb(c,0,0,0);cairo_paint(c);}}
void wpd_transition_draw(cairo_t*c,cairo_surface_t*o,cairo_surface_t*n,WpdTransition t,double p,int w,int h){p=CLAMP(p,0,1);paint(c,o);if(p>=1){paint(c,n);return;}cairo_save(c);if(t==WPD_TRANS_FADE){cairo_set_source_surface(c,n,0,0);cairo_paint_with_alpha(c,p);cairo_restore(c);return;}if(t==WPD_TRANS_WIPE)cairo_rectangle(c,0,0,w*p,h);else if(t==WPD_TRANS_SLIDE){cairo_rectangle(c,0,0,w*p,h);cairo_clip(c);cairo_set_source_surface(c,n,-w*(1-p),0);cairo_paint(c);cairo_restore(c);return;}else if(t==WPD_TRANS_GROW){cairo_translate(c,w/2.0,h/2.0);cairo_scale(c,p,p);cairo_translate(c,-w/2.0,-h/2.0);cairo_rectangle(c,0,0,w,h);}else if(t==WPD_TRANS_CENTER)cairo_rectangle(c,w*(1-p)/2,0,w*p,h);else if(t==WPD_TRANS_CORNER){double r=hypot(w,h)*p;cairo_arc(c,0,0,r,0,2*M_PI);}else if(t==WPD_TRANS_INFECTION){double cell=MAX(24,MIN(w,h)/24.0);for(double y=0;y<h;y+=cell)for(double x=0;x<w;x+=cell){double noise=fmod(sin(x*12.9898+y*78.233)*43758.5453,1.0);noise=fabs(noise);double d=hypot(x-w*.5,y-h*.5)/hypot(w*.5,h*.5);if(d*.72+noise*.28<p)cairo_arc(c,x+cell/2,y+cell/2,cell*(.55+p),0,2*M_PI);}}cairo_clip(c);paint(c,n);cairo_restore(c);}
