#pragma once
#include <glib.h>
GPtrArray *wpd_wallpapers_scan(const gchar *directory);
gboolean wpd_image_extension_supported(const gchar *path);
