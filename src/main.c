#include "wpd-build-config.h"
#include "config.h"
#include "daemon.h"
#include "ipc.h"
#include "image.h"
#include "transitions.h"
#include "gui/switcher.h"
#include "gui/carousel.h"
#include "gui/three_d.h"
#include "gui/alpha.h"
#include "gui/layouts.h"
#include <gtk/gtk.h>

static void help(void) {
  g_print("WPD %s - Wayland wallpaper daemon\n\n"
          "Usage:\n"
          "  wpd daemon\n"
          "  wpd image PATH [fill|fit|stretch|none]\n"
          "  wpd switcher\n"
          "  wpd carousel\n"
          "  wpd 3d\n"
          "  wpd grid\n"
          "  wpd stack\n"
          "  wpd filmstrip\n"
          "  wpd roots\n"
          "  wpd layouts\n"
          "  wpd theme [light|dark]\n"
          "  wpd transition [mode|list]\n"
          "  wpd --alpha\n"
          "  wpd help\n\n"
          "Options:\n"
          "  -h, --help     Show this help\n"
          "  -v, --version  Show version\n", WPD_VERSION);
}

static int gui_init(int *argc, char ***argv) {
  if (gtk_init_check(argc, argv)) return 0;
  g_printerr("wpd: cannot connect to a graphical display\n");
  return 1;
}

static int send_image(WpdConfig *config, int argc, char **argv) {
  if (argc < 3 || argc > 4) {
    g_printerr("usage: wpd image PATH [fill|fit|stretch|none]\n"); return 2;
  }
  WpdScaleMode mode;
  const gchar *mode_name = argc == 4 ? argv[3] : "fill";
  if (!wpd_scale_mode_parse(mode_name, &mode)) {
    g_printerr("wpd: invalid scaling mode\n"); return 2;
  }
  if (!config->socket_path) {
    g_printerr("wpd: XDG_RUNTIME_DIR is not set\n"); return 1;
  }
  gchar *path = g_canonicalize_filename(argv[2], NULL);
  gchar *request = g_strdup_printf("IMAGE\t%s\t%s", mode_name, path);
  gchar *reply = NULL; GError *error = NULL; int result = 0;
  if (!wpd_ipc_send(config->socket_path, request, &reply, &error)) {
    g_printerr("wpd: daemon unavailable: %s\n", error->message);
    g_clear_error(&error); result = 1;
  } else if (!g_str_has_prefix(reply, "OK")) {
    g_printerr("wpd: %s\n", reply); result = 1;
  }
  g_free(reply); g_free(request); g_free(path);
  return result;
}

int main(int argc, char **argv) {
  if (argc < 2 || !g_strcmp0(argv[1], "help") ||
      !g_strcmp0(argv[1], "--help") || !g_strcmp0(argv[1], "-h")) {
    help(); return 0;
  }
  if (!g_strcmp0(argv[1], "--version") || !g_strcmp0(argv[1], "-v")) {
    g_print("wpd %s\n", WPD_VERSION); return 0;
  }
  WpdConfig *config = wpd_config_new();
  int result;
  if (!g_strcmp0(argv[1],"--alpha")) {
    if (argc!=2) result=2;
    else if (g_random_int_range(0,100)>=31) {
      g_print("wpd: alpha behaved itself this time\n"); result=0;
    } else {
      result=gui_init(&argc,&argv);
      if (!result) result=wpd_alpha_run();
    }
  } else if (!g_strcmp0(argv[1], "daemon")) {
    if (argc!=2) result=2;
    else {
      gchar *reply=NULL; GError *error=NULL;
      gboolean running=config->socket_path &&
        wpd_ipc_send(config->socket_path,"PING",&reply,&error);
      g_free(reply); g_clear_error(&error);
      if (running) {
        g_print("wpd: daemon already running\n"); result=0;
      } else {
        int detached=wpd_daemon_detach(config);
        if (detached>0) result=0;
        else if (detached<0) result=1;
        else {
          result=gui_init(&argc,&argv);
          if (!result) result=wpd_daemon_run(config);
        }
      }
    }
  } else if (!g_strcmp0(argv[1], "image")) {
    result = send_image(config, argc, argv);
  } else if (!g_strcmp0(argv[1],"theme")) {
    if (argc==2) {
      g_print("%s\n",wpd_theme_get(config)); result=0;
    } else if (argc==3) {
      GError *error=NULL;
      if (wpd_theme_set(config,argv[2],&error)) {
        g_print("theme: %s\n",argv[2]); result=0;
      } else {
        g_printerr("wpd: %s\n",error->message); g_clear_error(&error); result=2;
      }
    } else {
      g_printerr("usage: wpd theme [light|dark]\n"); result=2;
    }
  } else if (!g_strcmp0(argv[1],"transition")) {
    if (argc==2) {
      g_print("%s\n",config->transition); result=0;
    } else if (argc==3 && !g_strcmp0(argv[2],"list")) {
      gsize count; const gchar *const *names=wpd_transition_names(&count);
      for (gsize i=0;i<count;i++) g_print("%s%s",names[i],i+1<count?" ":"\n");
      g_print("modifiers: invert left right top bottom topleft topright btmleft btmright\n");
      result=0;
    } else if (argc==3 && wpd_transition_valid(argv[2])) {
      GError *error=NULL;
      if (!wpd_transition_set(config,argv[2],&error)) {
        g_printerr("wpd: %s\n",error->message); g_clear_error(&error); result=1;
      } else {
        gchar *request=g_strdup_printf("TRANSITION\t%s",argv[2]), *reply=NULL;
        GError *ipc_error=NULL;
        if (config->socket_path)
          wpd_ipc_send(config->socket_path,request,&reply,&ipc_error);
        g_clear_error(&ipc_error); g_free(reply); g_free(request);
        g_print("transition: %s\n",argv[2]); result=0;
      }
    } else {
      g_printerr("wpd: unknown transition\nTry 'wpd transition list'.\n"); result=2;
    }
  } else if (!g_strcmp0(argv[1],"layouts")) {
    if (argc!=2) result=2;
    else { g_print("switcher carousel 3d grid stack filmstrip roots\n"); result=0; }
  } else if (!g_strcmp0(argv[1], "switcher") || !g_strcmp0(argv[1], "carousel") ||
             !g_strcmp0(argv[1], "3d") || !g_strcmp0(argv[1],"grid") ||
             !g_strcmp0(argv[1],"stack") || !g_strcmp0(argv[1],"filmstrip") ||
             !g_strcmp0(argv[1],"roots")) {
    result = argc != 2 ? 2 : gui_init(&argc, &argv);
    if (!result) {
      if (!g_strcmp0(argv[1],"switcher")) result=wpd_switcher_run(config);
      else if (!g_strcmp0(argv[1],"carousel")) result=wpd_carousel_run(config);
      else if (!g_strcmp0(argv[1],"3d")) result=wpd_three_d_run(config);
      else if (!g_strcmp0(argv[1],"grid")) result=wpd_grid_run(config);
      else if (!g_strcmp0(argv[1],"stack")) result=wpd_stack_run(config);
      else if (!g_strcmp0(argv[1],"filmstrip")) result=wpd_filmstrip_run(config);
      else result=wpd_roots_run(config);
    }
  } else {
    g_printerr("wpd: unknown command '%s'\nTry 'wpd help'.\n", argv[1]);
    result = 2;
  }
  wpd_config_free(config);
  return result;
}
