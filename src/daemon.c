#include "daemon.h"
#include "ipc.h"
#include "renderer.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  WpdConfig *c;
  WpdRenderer *r;
} Daemon;

int wpd_daemon_detach(const WpdConfig *config) {
  pid_t child=fork();
  if (child<0) { g_printerr("wpd: cannot fork daemon: %s\n",g_strerror(errno)); return -1; }
  if (child>0) {
    int status;
    while (waitpid(child,&status,0)<0 && errno==EINTR) {}
    return 1;
  }
  if (setsid()<0) _exit(1);
  pid_t grandchild=fork();
  if (grandchild<0) _exit(1);
  if (grandchild>0) _exit(0);

  g_mkdir_with_parents(config->state_dir,0700);
  gchar *log_path=g_build_filename(config->state_dir,"daemon.log",NULL);
  int null_fd=open("/dev/null",O_RDWR);
  int log_fd=open(log_path,O_WRONLY|O_CREAT|O_APPEND,0600);
  g_free(log_path);
  if (null_fd>=0) {
    dup2(null_fd,STDIN_FILENO); dup2(null_fd,STDOUT_FILENO);
    if (null_fd>STDERR_FILENO) close(null_fd);
  }
  if (log_fd>=0) {
    dup2(log_fd,STDERR_FILENO);
    if (log_fd>STDERR_FILENO) close(log_fd);
  }
  return 0;
}

typedef struct {
  gchar **argv;
  gchar *status_path;
  gchar *theme_path;
} MatugenJob;

static void matugen_job_free(MatugenJob *job) {
  g_strfreev(job->argv);
  g_free(job->status_path);
  g_free(job->theme_path);
  g_free(job);
}

static gchar *json_color(const gchar *json, const gchar *name) {
  const gchar *colors=strstr(json,"\"colors\"");
  gchar *needle=g_strdup_printf("\"%s\"",name);
  const gchar *position=colors?strstr(colors,needle):NULL;
  g_free(needle);
  if (!position || !(position=strstr(position,"\"default\"")) ||
      !(position=strstr(position,"\"color\"")) || !(position=strchr(position,'#')))
    return NULL;
  for (gint i=1;i<=6;i++) if (!g_ascii_isxdigit(position[i])) return NULL;
  return g_strndup(position,7);
}

static void write_matugen_theme(const MatugenJob *job, const gchar *json) {
  gchar *background=json_color(json,"surface");
  gchar *foreground=json_color(json,"on_surface");
  gchar *selected=json_color(json,"primary");
  gchar *border=json_color(json,"outline");
  gchar *shadow=json_color(json,"shadow");
  if (background && foreground && selected && border) {
    gchar *directory=g_path_get_dirname(job->theme_path);
    g_mkdir_with_parents(directory,0700);
    g_free(directory);
    const gchar *keys[]={"background_color","foreground_color","selected_color",
                         "border_color","shadow_color"};
    const gchar *values[]={background,foreground,selected,border,
                           shadow?shadow:"#000000"};
    gboolean found[5]={FALSE,FALSE,FALSE,FALSE,FALSE};
    gchar *existing=NULL;
    g_file_get_contents(job->theme_path,&existing,NULL,NULL);
    gchar **lines=g_strsplit(existing?existing:"","\n",-1);
    GString *output=g_string_new(NULL);
    for (guint i=0;lines[i];i++) {
      gboolean replaced=FALSE;
      for (guint k=0;k<5;k++) {
        gsize length=strlen(keys[k]);
        if (g_str_has_prefix(lines[i],keys[k]) && lines[i][length]=='=') {
          g_string_append_printf(output,"%s=%s\n",keys[k],values[k]);
          found[k]=TRUE; replaced=TRUE; break;
        }
      }
      if (!replaced && *lines[i]) g_string_append_printf(output,"%s\n",lines[i]);
    }
    for (guint k=0;k<5;k++)
      if (!found[k]) g_string_append_printf(output,"%s=%s\n",keys[k],values[k]);
    g_file_set_contents(job->theme_path,output->str,-1,NULL);
    g_string_free(output,TRUE); g_strfreev(lines); g_free(existing);
  }
  g_free(background); g_free(foreground); g_free(selected); g_free(border); g_free(shadow);
}

static void matugen_worker(GTask *task, gpointer source, gpointer task_data,
                           GCancellable *cancel) {
  (void)source; (void)cancel;
  MatugenJob *job=task_data;
  gchar *stdout_text=NULL, *stderr_text=NULL;
  gint status=0;
  GError *error=NULL;
  gboolean started=g_spawn_sync(NULL,job->argv,NULL,
    G_SPAWN_SEARCH_PATH|G_SPAWN_STDIN_FROM_DEV_NULL,NULL,NULL,
    &stdout_text,&stderr_text,&status,&error);
  if (started && !g_spawn_check_wait_status(status,&error)) started=FALSE;
  if (started && stdout_text) write_matugen_theme(job,stdout_text);
  gchar *report=started ? g_strdup("ok\n") :
    g_strdup_printf("error: %s%s%s\n",error?error->message:"unknown failure",
                    stderr_text&&*stderr_text?"\n":"",
                    stderr_text&&*stderr_text?stderr_text:"");
  g_file_set_contents(job->status_path,report,-1,NULL);
  g_free(report); g_free(stdout_text); g_free(stderr_text);
  if (started) g_task_return_boolean(task,TRUE);
  else g_task_return_error(task,error);
}

static void matugen_done(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; (void)data;
  GError *error=NULL;
  if (!g_task_propagate_boolean(G_TASK(result),&error)) {
    g_warning("Matugen hook: %s",error->message);
    g_clear_error(&error);
  }
}

static gboolean has_argument(GPtrArray *args, const gchar *name) {
  for (guint i=0; i<args->len; i++) {
    const gchar *arg=g_ptr_array_index(args,i);
    if (!g_strcmp0(arg,name) ||
        (g_str_has_prefix(arg,name) && arg[strlen(name)]=='='))
      return TRUE;
  }
  return FALSE;
}

static void run_matugen(WpdConfig *c, const gchar *path) {
  if (!c->matugen_command || !*c->matugen_command) return;
  GError *error = NULL;
  gint argc = 0;
  gchar **argv = NULL;
  if (!g_shell_parse_argv(c->matugen_command, &argc, &argv, &error)) {
    g_warning("invalid matugen_command: %s", error->message);
    g_clear_error(&error);
    return;
  }
  GPtrArray *args = g_ptr_array_new();
  for (gint i = 0; i < argc; i++)
    g_ptr_array_add(args, g_strdup(!g_strcmp0(argv[i], "%f") ? path : argv[i]));
  gchar *program=g_path_get_basename(argv[0]);
  if (!g_strcmp0(program,"matugen")) {
    if (!has_argument(args,"--source-color-index") &&
        !has_argument(args,"--prefer")) {
      g_ptr_array_add(args,g_strdup("--source-color-index"));
      g_ptr_array_add(args,g_strdup("0"));
    }
    if (!has_argument(args,"--quiet") && !has_argument(args,"-q"))
      g_ptr_array_add(args,g_strdup("--quiet"));
    if (!has_argument(args,"--json") && !has_argument(args,"-j")) {
      g_ptr_array_add(args,g_strdup("--json"));
      g_ptr_array_add(args,g_strdup("hex"));
    }
    if (!has_argument(args,"--mode") && !has_argument(args,"-m")) {
      g_ptr_array_add(args,g_strdup("--mode"));
      g_ptr_array_add(args,g_strdup(wpd_theme_get(c)));
    }
  }
  g_free(program);
  g_ptr_array_add(args,NULL);
  MatugenJob *job=g_new0(MatugenJob,1);
  job->argv=(gchar**)g_ptr_array_free(args,FALSE);
  job->status_path=g_build_filename(c->state_dir,"matugen-status",NULL);
  job->theme_path=g_build_filename(c->config_dir,"switcher.conf",NULL);
  GTask *task=g_task_new(NULL,NULL,matugen_done,NULL);
  g_task_set_task_data(task,job,(GDestroyNotify)matugen_job_free);
  g_task_run_in_thread(task,matugen_worker);
  g_object_unref(task);
  g_strfreev(argv);
}

static gchar *handle(const gchar *request, gpointer data) {
  Daemon *d = data;
  gchar **v = g_strsplit(request, "\t", 4);
  if (!g_strcmp0(v[0],"PING")) { g_strfreev(v); return g_strdup("OK"); }
  if (!g_strcmp0(v[0],"TRANSITION") && v[1]) {
    if (!wpd_transition_valid(v[1])) { g_strfreev(v); return g_strdup("ERR bad transition"); }
    g_free(d->c->transition); d->c->transition=g_strdup(v[1]);
    g_strfreev(v); return g_strdup("OK");
  }
  if (g_strcmp0(v[0], "IMAGE") || !v[1] || !v[2]) {
    g_strfreev(v);
    return g_strdup("ERR bad command");
  }
  WpdScaleMode mode;
  if (!wpd_scale_mode_parse(v[1], &mode)) {
    g_strfreev(v);
    return g_strdup("ERR bad scaling mode");
  }
  gchar *path = g_canonicalize_filename(v[2], NULL);
  if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
    g_free(path); g_strfreev(v);
    return g_strdup("ERR image not found");
  }
  GError *error = NULL;
  WpdTransition transition = wpd_transition_parse(v[3] ? v[3] : d->c->transition);
  if (!wpd_renderer_set(d->r, path, mode, transition, &error)) {
    gchar *reply = g_strdup_printf("ERR %s", error->message);
    g_clear_error(&error); g_free(path); g_strfreev(v);
    return reply;
  }
  g_mkdir_with_parents(d->c->state_dir, 0700);
  gchar *state = g_strdup_printf("%s\t%s\n", v[1], path);
  g_file_set_contents(d->c->state_file, state, -1, NULL);
  g_free(state);
  run_matugen(d->c,path);
  g_free(path); g_strfreev(v);
  return g_strdup("OK");
}

int wpd_daemon_run(WpdConfig *c) {
  if (!c->runtime_dir) { g_printerr("wpd: XDG_RUNTIME_DIR is not set\n"); return 1; }
  if (g_mkdir_with_parents(c->runtime_dir, 0700) < 0) {
    g_printerr("wpd: cannot create runtime directory\n"); return 1;
  }
  if (g_file_test(c->socket_path, G_FILE_TEST_EXISTS)) {
    gchar *reply = NULL; GError *error = NULL;
    if (wpd_ipc_send(c->socket_path, "PING", &reply, &error)) {
      g_printerr("wpd: daemon already running\n"); g_free(reply); return 1;
    }
    g_clear_error(&error); g_unlink(c->socket_path);
  }
  Daemon d = {.c=c, .r=wpd_renderer_new(c->duration_ms, c->fps)};
  GError *error = NULL;
  GSocketService *service = wpd_ipc_listen(c->socket_path, handle, &d, &error);
  if (!service) {
    g_printerr("wpd: %s\n", error->message); g_clear_error(&error);
    wpd_renderer_free(d.r); return 1;
  }
  chmod(c->socket_path, 0600);
  gchar *saved = NULL;
  if (g_file_get_contents(c->state_file, &saved, NULL, NULL)) {
    g_strchomp(saved); gchar **v = g_strsplit(saved, "\t", 2); WpdScaleMode mode;
    if (v[1] && g_file_test(v[1], G_FILE_TEST_EXISTS) && wpd_scale_mode_parse(v[0], &mode))
      wpd_renderer_set(d.r, v[1], mode, WPD_TRANS_FADE, NULL);
    g_strfreev(v); g_free(saved);
  }
  gtk_main();
  g_object_unref(service); g_unlink(c->socket_path); wpd_renderer_free(d.r);
  return 0;
}
