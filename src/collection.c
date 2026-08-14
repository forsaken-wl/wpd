#include "collection.h"
#include "ipc.h"
#include "wallpaper.h"
#include <errno.h>

static gint name_compare(gconstpointer left,gconstpointer right) {
  return g_utf8_collate(*(const gchar *const *)left,*(const gchar *const *)right);
}

static gchar *collection_path(const WpdConfig *config,const gchar *name,GError **error) {
  if (!name || !*name) return g_strdup(config->papers_dir);
  if (strchr(name,'/') || !g_strcmp0(name,".") || !g_strcmp0(name,"..")) {
    g_set_error(error,G_OPTION_ERROR,G_OPTION_ERROR_BAD_VALUE,
                "collection must be one folder name");return NULL;
  }
  gchar *path=g_build_filename(config->papers_dir,name,NULL);
  if (!g_file_test(path,G_FILE_TEST_IS_DIR)) {
    g_set_error(error,G_FILE_ERROR,G_FILE_ERROR_NOENT,
                "collection '%s' does not exist",name);g_free(path);return NULL;
  }
  return path;
}

static gchar *collection_theme(const gchar *directory) {
  gchar *path=g_build_filename(directory,".wpd.conf",NULL),*data=NULL,*theme=NULL;
  if (g_file_get_contents(path,&data,NULL,NULL)) {
    gchar **lines=g_strsplit(data,"\n",-1);
    for(guint i=0;lines[i];i++) {
      gchar *line=g_strstrip(lines[i]);
      if(g_str_has_prefix(line,"theme=")) {
        const gchar *value=g_strstrip(line+6);
        if(!g_strcmp0(value,"light")||!g_strcmp0(value,"dark"))theme=g_strdup(value);
        break;
      }
    }
    g_strfreev(lines);
  }
  g_free(data);g_free(path);return theme;
}

gboolean wpd_collection_set_theme(const WpdConfig *config,const gchar *name,
                                  const gchar *theme,GError **error) {
  if(g_strcmp0(theme,"light")&&g_strcmp0(theme,"dark")) {
    g_set_error(error,G_OPTION_ERROR,G_OPTION_ERROR_BAD_VALUE,"theme must be light or dark");
    return FALSE;
  }
  gchar *directory=collection_path(config,name,error);if(!directory)return FALSE;
  gchar *path=g_build_filename(directory,".wpd.conf",NULL);
  gchar *contents=g_strdup_printf("# WPD collection behavior\ntheme=%s\n",theme);
  gboolean ok=g_file_set_contents(path,contents,-1,error);
  g_free(contents);g_free(path);g_free(directory);return ok;
}

static gchar *current_path(const WpdConfig *config) {
  gchar *state=NULL;if(!g_file_get_contents(config->state_file,&state,NULL,NULL))return NULL;
  g_strchomp(state);gchar **parts=g_strsplit(state,"\t",2);
  gchar *path=parts[1]?g_strdup(parts[1]):NULL;g_strfreev(parts);g_free(state);return path;
}

int wpd_collection_apply(WpdConfig *config,const gchar *name,const gchar *action) {
  GError *error=NULL;gchar *directory=collection_path(config,name,&error);
  if(!directory){g_printerr("wpd: %s\n",error->message);g_clear_error(&error);return 1;}
  GPtrArray *papers=wpd_wallpapers_scan(directory);
  if(!papers->len){g_printerr("wpd: collection '%s' has no wallpapers\n",name?name:"all");g_ptr_array_free(papers,TRUE);g_free(directory);return 1;}
  gchar *current=current_path(config);gint index=-1;
  for(guint i=0;i<papers->len;i++)if(!g_strcmp0(current,g_ptr_array_index(papers,i))){index=(gint)i;break;}
  if(!g_strcmp0(action,"random")) {
    gint old=index;index=g_random_int_range(0,(gint)papers->len);
    if(papers->len>1&&index==old)index=(index+1)%(gint)papers->len;
  } else if(!g_strcmp0(action,"next")) index=(index+1+(gint)papers->len)%(gint)papers->len;
  else index=(index<0?(gint)papers->len-1:index-1+(gint)papers->len)%(gint)papers->len;
  gchar *theme=collection_theme(directory);
  if(theme&&!wpd_theme_set(config,theme,&error)) {
    g_printerr("wpd: cannot apply collection theme: %s\n",error->message);g_clear_error(&error);
  }
  const gchar *paper=g_ptr_array_index(papers,index);
  gchar *request=g_strdup_printf("IMAGE\tfill\t%s",paper),*reply=NULL;int result=0;
  if(!config->socket_path||!wpd_ipc_send(config->socket_path,request,&reply,&error)) {
    g_printerr("wpd: daemon unavailable: %s\n",error?error->message:"no runtime directory");
    g_clear_error(&error);result=1;
  } else if(!g_str_has_prefix(reply,"OK")){g_printerr("wpd: %s\n",reply);result=1;}
  else g_print("%s\n",paper);
  g_free(reply);g_free(request);g_free(theme);g_free(current);
  g_ptr_array_free(papers,TRUE);g_free(directory);return result;
}

void wpd_collections_print(const WpdConfig *config) {
  GDir *dir=g_dir_open(config->papers_dir,0,NULL);if(!dir){g_print("No collections.\n");return;}
  GPtrArray *names=g_ptr_array_new_with_free_func(g_free);
  const gchar *name;while((name=g_dir_read_name(dir))) {
    gchar *path=g_build_filename(config->papers_dir,name,NULL);
    if(g_file_test(path,G_FILE_TEST_IS_DIR)&&!g_file_test(path,G_FILE_TEST_IS_SYMLINK))
      g_ptr_array_add(names,g_strdup(name));
    g_free(path);
  }
  g_dir_close(dir);
  g_ptr_array_sort(names,name_compare);
  for(guint i=0;i<names->len;i++) {
    name=g_ptr_array_index(names,i);gchar *path=g_build_filename(config->papers_dir,name,NULL);
    gchar *theme=collection_theme(path);g_print("%s%s%s\n",name,theme?"  ":"",theme?theme:"");
    g_free(theme);g_free(path);
  }
  g_ptr_array_free(names,TRUE);
}
