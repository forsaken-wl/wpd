#include "video.h"
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <string.h>

struct _WmdVideo {
  WpdRenderer *renderer;
  GstElement *pipeline, *sink;
  guint bus_watch, generation;
  gint pending;
};

typedef struct {
  WmdVideo *video;
  cairo_surface_t *surface;
  guint generation;
} Frame;

static gboolean deliver_frame(gpointer data) {
  Frame *frame=data;
  if (frame->generation==frame->video->generation)
    wpd_renderer_set_video_frame(frame->video->renderer,frame->surface);
  else
    cairo_surface_destroy(frame->surface);
  g_atomic_int_set(&frame->video->pending,0);
  g_free(frame);
  return G_SOURCE_REMOVE;
}

static GstFlowReturn new_sample(GstAppSink *sink,gpointer data) {
  WmdVideo *video=data;
  GstSample *sample=gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_EOS;
  if (!g_atomic_int_compare_and_exchange(&video->pending,0,1)) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  GstVideoInfo info;
  if (!gst_video_info_from_caps(&info,gst_sample_get_caps(sample))) {
    g_atomic_int_set(&video->pending,0);
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }
  GstBuffer *buffer=gst_sample_get_buffer(sample);
  GstMapInfo map;
  if (!gst_buffer_map(buffer,&map,GST_MAP_READ)) {
    g_atomic_int_set(&video->pending,0);
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }

  gint width=GST_VIDEO_INFO_WIDTH(&info),height=GST_VIDEO_INFO_HEIGHT(&info);
  gint source_stride=GST_VIDEO_INFO_PLANE_STRIDE(&info,0);
  cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,width,height);
  guchar *destination=cairo_image_surface_get_data(surface);
  gint stride=cairo_image_surface_get_stride(surface);
  for (gint y=0;y<height;y++)
    memcpy(destination+y*stride,map.data+y*source_stride,MIN(stride,source_stride));
  cairo_surface_mark_dirty(surface);
  gst_buffer_unmap(buffer,&map);
  gst_sample_unref(sample);

  Frame *frame=g_new(Frame,1);
  frame->video=video;
  frame->surface=surface;
  frame->generation=video->generation;
  g_main_context_invoke(NULL,deliver_frame,frame);
  return GST_FLOW_OK;
}

static gboolean bus_message(GstBus *bus,GstMessage *message,gpointer data) {
  (void)bus;
  WmdVideo *video=data;
  if (GST_MESSAGE_TYPE(message)==GST_MESSAGE_EOS) {
    gst_element_seek_simple(video->pipeline,GST_FORMAT_TIME,
      GST_SEEK_FLAG_FLUSH|GST_SEEK_FLAG_KEY_UNIT,0);
  } else if (GST_MESSAGE_TYPE(message)==GST_MESSAGE_ERROR) {
    GError *error=NULL;
    gchar *debug=NULL;
    gst_message_parse_error(message,&error,&debug);
    g_warning("video: %s",error->message);
    g_clear_error(&error);
    g_free(debug);
  }
  return G_SOURCE_CONTINUE;
}

WmdVideo *wmd_video_new(WpdRenderer *renderer) {
  WmdVideo *video=g_new0(WmdVideo,1);
  video->renderer=renderer;
  return video;
}

void wmd_video_stop(WmdVideo *video) {
  if (!video) return;
  video->generation++;
  if (video->pipeline) gst_element_set_state(video->pipeline,GST_STATE_NULL);
  if (video->bus_watch) {
    g_source_remove(video->bus_watch);
    video->bus_watch=0;
  }
  g_clear_object(&video->pipeline);
  video->sink=NULL;
}

gboolean wmd_video_play(WmdVideo *video,const gchar *path,WpdScaleMode mode,
                        GError **error) {
  wmd_video_stop(video);
  video->pipeline=gst_element_factory_make("playbin",NULL);
  video->sink=gst_element_factory_make("appsink",NULL);
  GstElement *audio=gst_element_factory_make("fakesink",NULL);
  if (!video->pipeline || !video->sink || !audio) {
    g_set_error(error,G_FILE_ERROR,G_FILE_ERROR_FAILED,
                "missing GStreamer playback elements");
    if (audio) gst_object_unref(audio);
    wmd_video_stop(video);
    return FALSE;
  }

  GstCaps *caps=gst_caps_new_simple("video/x-raw","format",G_TYPE_STRING,"BGRA",NULL);
  g_object_set(video->sink,"caps",caps,"max-buffers",1,"drop",TRUE,
               "sync",TRUE,"emit-signals",TRUE,NULL);
  gst_caps_unref(caps);
  g_signal_connect(video->sink,"new-sample",G_CALLBACK(new_sample),video);

  gchar *uri=g_filename_to_uri(path,NULL,error);
  if (!uri) {
    gst_object_unref(audio);
    wmd_video_stop(video);
    return FALSE;
  }
  g_object_set(video->pipeline,"uri",uri,"video-sink",video->sink,
               "audio-sink",audio,NULL);
  g_free(uri);
  gst_object_unref(audio);

  GstBus *bus=gst_element_get_bus(video->pipeline);
  video->bus_watch=gst_bus_add_watch(bus,bus_message,video);
  gst_object_unref(bus);
  wpd_renderer_begin_video(video->renderer,mode);
  if (gst_element_set_state(video->pipeline,GST_STATE_PLAYING)==
      GST_STATE_CHANGE_FAILURE) {
    g_set_error(error,G_FILE_ERROR,G_FILE_ERROR_FAILED,"GStreamer refused playback");
    wmd_video_stop(video);
    return FALSE;
  }
  return TRUE;
}

void wmd_video_free(WmdVideo *video) {
  if (!video) return;
  wmd_video_stop(video);
  g_free(video);
}
