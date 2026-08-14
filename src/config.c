#include "config.h"
#include <errno.h>

static gchar *base_path(const gchar *env, const gchar *fallback) {
  const gchar *v = g_getenv(env);
  return (v && *v) ? g_strdup(v) : g_build_filename(g_get_home_dir(), fallback, NULL);
}

static void parse_daemon(WpdConfig *c) {
  gchar *path = g_build_filename(c->config_dir, "switcher.conf", NULL);
  gchar *data = NULL; gsize len = 0;
  if (!g_file_get_contents(path, &data, &len, NULL)) { g_free(path); return; }
  gchar **lines = g_strsplit(data, "\n", -1);
  for (guint i=0; lines[i]; i++) {
    gchar *s = g_strstrip(lines[i]); if (!*s || *s=='#') continue;
    gchar **kv = g_strsplit(s, "=", 2); if (!kv[1]) { g_strfreev(kv); continue; }
    gchar *k=g_strstrip(kv[0]), *v=g_strstrip(kv[1]);
    if (!g_strcmp0(k,"matugen_command")) { g_free(c->matugen_command); c->matugen_command=g_strdup(v); }
    else if (!g_strcmp0(k,"transition")) { g_free(c->transition); c->transition=g_strdup(v); }
    else if (!g_strcmp0(k,"duration_ms")) { guint64 n=g_ascii_strtoull(v,NULL,10); if(n>=50&&n<=10000)c->duration_ms=n; }
    else if (!g_strcmp0(k,"fps")) { guint64 n=g_ascii_strtoull(v,NULL,10); if(n>=1&&n<=240)c->fps=n; }
    g_strfreev(kv);
  }
  g_strfreev(lines); g_free(data); g_free(path);
}

WpdConfig *wpd_config_new(void) {
  WpdConfig *c=g_new0(WpdConfig,1);
  c->config_dir=g_build_filename((g_getenv("XDG_CONFIG_HOME")&&*g_getenv("XDG_CONFIG_HOME"))?g_getenv("XDG_CONFIG_HOME"):g_get_user_config_dir(),"wpd",NULL);
  c->papers_dir=g_build_filename(c->config_dir,"papers",NULL);
  gchar *cache=base_path("XDG_CACHE_HOME",".cache"); c->cache_dir=g_build_filename(cache,"wpd",NULL); g_free(cache);
  gchar *state=base_path("XDG_STATE_HOME",".local/state"); c->state_dir=g_build_filename(state,"wpd",NULL); g_free(state);
  const gchar *runtime=g_getenv("XDG_RUNTIME_DIR");
  c->runtime_dir=runtime&&*runtime?g_build_filename(runtime,"wpd",NULL):NULL;
  c->socket_path=c->runtime_dir?g_build_filename(c->runtime_dir,"socket",NULL):NULL;
  c->state_file=g_build_filename(c->state_dir,"current",NULL);
  c->theme_file=g_build_filename(c->config_dir,"switcher.conf",NULL);
  c->matugen_command=g_strdup("matugen image %f --source-color-index 0 --quiet");
  c->transition=g_strdup("fade"); c->duration_ms=650; c->fps=60;
  parse_daemon(c); return c;
}
void wpd_config_free(WpdConfig*c){if(!c)return;g_free(c->config_dir);g_free(c->papers_dir);g_free(c->cache_dir);g_free(c->state_dir);g_free(c->runtime_dir);g_free(c->socket_path);g_free(c->state_file);g_free(c->theme_file);g_free(c->matugen_command);g_free(c->transition);g_free(c);}

const gchar *wpd_theme_get(const WpdConfig *config) {
  gchar *data=NULL;
  if (g_file_get_contents(config->theme_file,&data,NULL,NULL)) {
    gchar **lines=g_strsplit(data,"\n",-1);
    for (guint i=0;lines[i];i++) {
      gchar *line=g_strstrip(lines[i]);
      if (g_str_has_prefix(line,"theme=")) {
        gboolean light=!g_strcmp0(g_strstrip(line+6),"light");
        g_strfreev(lines); g_free(data);
        return light?"light":"dark";
      }
    }
    g_strfreev(lines); g_free(data);
  }
  return "dark";
}

gboolean wpd_theme_set(const WpdConfig *config, const gchar *theme, GError **error) {
  if (g_strcmp0(theme,"light") && g_strcmp0(theme,"dark")) {
    g_set_error(error,G_OPTION_ERROR,G_OPTION_ERROR_BAD_VALUE,
                "theme must be 'light' or 'dark'");
    return FALSE;
  }
  if (g_mkdir_with_parents(config->config_dir,0700)<0) {
    g_set_error(error,G_FILE_ERROR,g_file_error_from_errno(errno),
                "cannot create state directory");
    return FALSE;
  }
  gchar *data=NULL; g_file_get_contents(config->theme_file,&data,NULL,NULL);
  gchar **lines=g_strsplit(data?data:"","\n",-1); GString *contents=g_string_new(NULL);
  gboolean found=FALSE;
  for (guint i=0;lines[i];i++) {
    if (g_str_has_prefix(g_strstrip(lines[i]),"theme=")) {
      g_string_append_printf(contents,"theme=%s\n",theme); found=TRUE;
    } else if (*lines[i]) g_string_append_printf(contents,"%s\n",lines[i]);
  }
  if (!found) g_string_append_printf(contents,"theme=%s\n",theme);
  gboolean result=g_file_set_contents(config->theme_file,contents->str,-1,error);
  g_string_free(contents,TRUE); g_strfreev(lines); g_free(data);
  return result;
}

gboolean wpd_transition_set(const WpdConfig *config,const gchar *transition,GError **error) {
  if (g_mkdir_with_parents(config->config_dir,0700)<0) {
    g_set_error(error,G_FILE_ERROR,g_file_error_from_errno(errno),
                "cannot create config directory"); return FALSE;
  }
  gchar *data=NULL; g_file_get_contents(config->theme_file,&data,NULL,NULL);
  gchar **lines=g_strsplit(data?data:"","\n",-1); GString *contents=g_string_new(NULL);
  gboolean found=FALSE;
  for (guint i=0;lines[i];i++) {
    if (g_str_has_prefix(g_strstrip(lines[i]),"transition=")) {
      g_string_append_printf(contents,"transition=%s\n",transition); found=TRUE;
    } else if (*lines[i]) g_string_append_printf(contents,"%s\n",lines[i]);
  }
  if (!found) g_string_append_printf(contents,"transition=%s\n",transition);
  gboolean result=g_file_set_contents(config->theme_file,contents->str,-1,error);
  g_string_free(contents,TRUE); g_strfreev(lines); g_free(data); return result;
}

static gint sane_int(const gchar *value, gint low, gint high, gint fallback) {
  gchar *end = NULL;
  gint64 number = g_ascii_strtoll(value, &end, 10);
  return end && !*end && number >= low && number <= high ? (gint)number : fallback;
}

void wpd_switcher_config_load(const WpdConfig *config, SwitcherConfig *out) {
  *out = (SwitcherConfig){.band=TRUE,.light=FALSE,.skew=0,.radius=0,.dim=0,
    .spacing=142,.selected_width=280,.side_width=170,.height=160,.border=3};
  out->light=!g_strcmp0(wpd_theme_get(config),"light");
  gdk_rgba_parse(&out->background_color,out->light?"#f0f1f5ed":"#090a0ee6");
  gdk_rgba_parse(&out->foreground_color,out->light?"#151820":"#f3f5ffff");
  gdk_rgba_parse(&out->selected_color,out->light?"#263b63":"#d9e6ffff");
  gdk_rgba_parse(&out->border_color,out->light?"#555a66":"#404753e6");
  gdk_rgba_parse(&out->shadow_color,"#00000055");
  gchar *path = g_build_filename(config->config_dir,"switcher.conf",NULL);
  gchar *data = NULL; gsize length;
  if (!g_file_get_contents(path,&data,&length,NULL)) { g_free(path); return; }
  gchar **lines = g_strsplit(data,"\n",-1);
  for (guint i=0; lines[i]; i++) {
    gchar *line=g_strstrip(lines[i]);
    if (!*line || *line=='#') continue;
    gchar **pair=g_strsplit(line,"=",2);
    if (!pair[1]) { g_strfreev(pair); continue; }
    gchar *key=g_strstrip(pair[0]), *value=g_strstrip(pair[1]);
    if (!g_strcmp0(key,"background")) {
      if (!g_strcmp0(value,"band")) out->band=TRUE;
      else if (!g_strcmp0(value,"transparent")) out->band=FALSE;
    } else if (!g_strcmp0(key,"skew")) out->skew=sane_int(value,-100,100,out->skew);
    else if (!g_strcmp0(key,"radius")) out->radius=sane_int(value,0,100,out->radius);
    else if (!g_strcmp0(key,"dim")) out->dim=sane_int(value,0,100,out->dim);
    else if (!g_strcmp0(key,"spacing")) out->spacing=sane_int(value,20,1000,out->spacing);
    else if (!g_strcmp0(key,"selected_width")) out->selected_width=sane_int(value,40,1200,out->selected_width);
    else if (!g_strcmp0(key,"side_width")) out->side_width=sane_int(value,40,1200,out->side_width);
    else if (!g_strcmp0(key,"height")) out->height=sane_int(value,40,1000,out->height);
    else if (!g_strcmp0(key,"border")) out->border=sane_int(value,0,30,out->border);
    else if (!g_strcmp0(key,"background_color")) gdk_rgba_parse(&out->background_color,value);
    else if (!g_strcmp0(key,"foreground_color")) gdk_rgba_parse(&out->foreground_color,value);
    else if (!g_strcmp0(key,"selected_color")) gdk_rgba_parse(&out->selected_color,value);
    else if (!g_strcmp0(key,"border_color")) gdk_rgba_parse(&out->border_color,value);
    else if (!g_strcmp0(key,"shadow_color")) gdk_rgba_parse(&out->shadow_color,value);
    g_strfreev(pair);
  }
  g_strfreev(lines); g_free(data); g_free(path);
}
