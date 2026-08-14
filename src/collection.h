#pragma once
#include "config.h"

gboolean wpd_collection_set_theme(const WpdConfig *config,const gchar *name,
                                  const gchar *theme,GError **error);
int wpd_collection_apply(WpdConfig *config,const gchar *name,const gchar *action);
void wpd_collections_print(const WpdConfig *config);
