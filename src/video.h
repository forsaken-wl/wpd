#pragma once
#include "renderer.h"
typedef struct _WmdVideo WmdVideo;
WmdVideo *wmd_video_new(WpdRenderer *renderer);
gboolean wmd_video_play(WmdVideo *video,const gchar *path,WpdScaleMode mode,GError **error);
void wmd_video_stop(WmdVideo *video);
void wmd_video_free(WmdVideo *video);
