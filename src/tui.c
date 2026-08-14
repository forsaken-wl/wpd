#include "tui.h"
#include "ipc.h"
#include <stdio.h>

static void notify_daemon(WpdConfig *config,const gchar *key,const gchar *value) {
  gchar *request=g_strdup_printf("%s\t%s",key,value),*reply=NULL;
  GError *error=NULL;
  if(config->socket_path)wpd_ipc_send(config->socket_path,request,&reply,&error);
  g_clear_error(&error);g_free(reply);g_free(request);
}

int wmd_tui_run(WpdConfig *config) {
  gchar input[64];
  for (;;) {
    g_print("\033[2J\033[H"
      "┌────────────────────────────────┐\n"
      "│ WMD configuration              │\n"
      "├────────────────────────────────┤\n"
      "│ Render FPS: %-3u                │\n"
      "│ Hardware:   %-4s               │\n"
      "│                                │\n"
      "│ [f] FPS  [h] Hardware  [q] Quit│\n"
      "└────────────────────────────────┘\n> ",config->fps,config->hardware);
    fflush(stdout);
    if(!fgets(input,sizeof input,stdin)||input[0]=='q'||input[0]=='Q')break;
    if(input[0]=='f'||input[0]=='F') {
      g_print("FPS (1-240): ");fflush(stdout);
      if(!fgets(input,sizeof input,stdin))break;
      guint64 fps=g_ascii_strtoull(input,NULL,10);GError *error=NULL;
      if(!wmd_fps_set(config,fps,&error)) {
        g_printerr("wmd: %s\n",error->message);g_clear_error(&error);g_usleep(1200000);
      } else {
        gchar *value=g_strdup_printf("%u",config->fps);
        notify_daemon(config,"FPS",value);g_free(value);
      }
    } else if(input[0]=='h'||input[0]=='H') {
      const gchar *next=!g_strcmp0(config->hardware,"auto")?"on":
                        !g_strcmp0(config->hardware,"on")?"off":"auto";
      GError *error=NULL;
      if(wmd_hardware_set(config,next,&error))notify_daemon(config,"HARDWARE",next);
      else g_clear_error(&error);
    }
  }
  g_print("\033[2J\033[H");return 0;
}
