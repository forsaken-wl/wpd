#include "ipc.h"
#include <string.h>

typedef struct { WpdIpcHandler handler; gpointer data; } Context;

static void context_free(gpointer data, GClosure *closure) {
  (void)closure; g_free(data);
}

static gboolean incoming(GSocketService *service, GSocketConnection *connection,
                         GObject *source, gpointer data) {
  (void)service; (void)source;
  Context *context = data;
  GDataInputStream *input = g_data_input_stream_new(
    g_io_stream_get_input_stream(G_IO_STREAM(connection)));
  GError *error = NULL;
  gchar *line = g_data_input_stream_read_line(input, NULL, NULL, &error);
  gchar *reply = line ? context->handler(line, context->data)
                      : g_strdup_printf("ERR %s", error ? error->message : "empty request");
  GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  g_output_stream_write_all(output, reply, strlen(reply), NULL, NULL, NULL);
  g_output_stream_write_all(output, "\n", 1, NULL, NULL, NULL);
  g_output_stream_flush(output, NULL, NULL);
  g_free(reply); g_free(line); g_clear_error(&error); g_object_unref(input);
  return TRUE;
}

GSocketService *wpd_ipc_listen(const gchar *path, WpdIpcHandler handler,
                               gpointer data, GError **error) {
  GSocketService *service = g_socket_service_new();
  GSocketAddress *address = g_unix_socket_address_new(path);
  if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service), address,
      G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, error)) {
    g_object_unref(address); g_object_unref(service); return NULL;
  }
  g_object_unref(address);
  Context *context = g_new0(Context, 1);
  context->handler = handler; context->data = data;
  g_signal_connect_data(service, "incoming", G_CALLBACK(incoming), context,
                        context_free, 0);
  g_socket_service_start(service);
  return service;
}

gboolean wpd_ipc_send(const gchar *path, const gchar *request,
                      gchar **reply, GError **error) {
  GSocketClient *client = g_socket_client_new();
  GSocketAddress *address = g_unix_socket_address_new(path);
  GSocketConnection *connection = g_socket_client_connect(
    client, G_SOCKET_CONNECTABLE(address), NULL, error);
  g_object_unref(address); g_object_unref(client);
  if (!connection) return FALSE;
  GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  gchar *line = g_strconcat(request, "\n", NULL);
  gboolean ok = g_output_stream_write_all(output, line, strlen(line), NULL, NULL, error);
  g_free(line);
  if (ok) {
    GDataInputStream *input = g_data_input_stream_new(
      g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    *reply = g_data_input_stream_read_line(input, NULL, NULL, error);
    ok = *reply != NULL;
    g_object_unref(input);
  }
  g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
  g_object_unref(connection);
  return ok;
}
