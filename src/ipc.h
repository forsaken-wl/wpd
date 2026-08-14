#pragma once
#include <gio/gio.h>
typedef gchar *(*WpdIpcHandler)(const gchar *request,gpointer data);
GSocketService *wpd_ipc_listen(const gchar *socket_path,WpdIpcHandler handler,gpointer data,GError **error);
gboolean wpd_ipc_send(const gchar *socket_path,const gchar *request,gchar **reply,GError **error);
