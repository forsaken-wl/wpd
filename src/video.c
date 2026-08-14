#include "video.h"
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/pbutils/missing-plugins.h>
#include <string.h>

struct _WmdVideo {
  WpdRenderer *renderer;
  GstElement *pipeline, *sink, *capsfilter;
  guint bus_watch, generation;
  gint pending;
  guint fps;
  gint64 last_frame;
  gint max_width,max_height;
  gchar *hardware;
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
  gint64 now=g_get_monotonic_time();
  if(video->last_frame && now-video->last_frame<1000000/(gint64)video->fps){
    gst_sample_unref(sample);return GST_FLOW_OK;
  }
  if (!g_atomic_int_compare_and_exchange(&video->pending,0,1)) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }
  video->last_frame=now;

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
  /* g_main_context_invoke() may call directly when startup has not entered
     gtk_main() yet. An idle source always defers GTK/Cairo work to the main
     context, which makes saved-video restore thread-safe. */
  g_idle_add_full(G_PRIORITY_HIGH_IDLE,deliver_frame,frame,NULL);
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

static gboolean hardware_h264_available(void){GstElementFactory *factory=gst_element_factory_find("vah264dec");if(!factory)return FALSE;gst_object_unref(factory);return TRUE;}

WmdVideo *wmd_video_new(WpdRenderer *renderer) {
  WmdVideo *video=g_new0(WmdVideo,1);
  video->renderer=renderer;video->fps=60;video->hardware=g_strdup("auto");
  wpd_renderer_max_size(renderer,&video->max_width,&video->max_height);
  return video;
}

static void update_caps(WmdVideo *video){if(!video->capsfilter)return;GstCaps *caps=gst_caps_new_simple("video/x-raw","format",G_TYPE_STRING,"BGRA","width",GST_TYPE_INT_RANGE,1,video->max_width,"height",GST_TYPE_INT_RANGE,1,video->max_height,"framerate",GST_TYPE_FRACTION,video->fps,1,NULL);g_object_set(video->capsfilter,"caps",caps,NULL);gst_caps_unref(caps);}
void wmd_video_set_fps(WmdVideo *video,guint fps){video->fps=CLAMP(fps,1,240);video->last_frame=0;update_caps(video);}
void wmd_video_set_hardware(WmdVideo *video,const gchar *mode){g_free(video->hardware);video->hardware=g_strdup(mode);}

void wmd_video_stop(WmdVideo *video) {
  if (!video) return;
  video->generation++;
  if (video->pipeline) gst_element_set_state(video->pipeline,GST_STATE_NULL);
  if (video->bus_watch) {
    g_source_remove(video->bus_watch);
    video->bus_watch=0;
  }
  g_clear_object(&video->pipeline);
  g_clear_object(&video->sink);
  video->capsfilter=NULL;
}

gboolean wmd_video_play(WmdVideo *video,const gchar *path,WpdScaleMode mode,
                        GError **error) {
  wmd_video_stop(video);
  video->last_frame=0;
  video->pipeline=gst_element_factory_make("playbin",NULL);
  video->sink=gst_element_factory_make("appsink",NULL);
  GstElement *audio=gst_element_factory_make("fakesink",NULL);
  GstElement *video_sink=gst_bin_new(NULL),*queue=gst_element_factory_make("queue",NULL);
  GstElement *rate=gst_element_factory_make("videorate",NULL);
  GstElement *scale=gst_element_factory_make("videoscale",NULL);
  GstElement *convert=gst_element_factory_make("videoconvert",NULL);
  video->capsfilter=gst_element_factory_make("capsfilter",NULL);
  if (!video->pipeline || !video->sink || !audio || !video_sink || !queue ||
      !rate || !scale || !convert || !video->capsfilter) {
    g_set_error(error,G_FILE_ERROR,G_FILE_ERROR_FAILED,
                "missing GStreamer playback elements");
    if (audio) gst_object_unref(audio);
    if (video_sink) gst_object_unref(video_sink);
    wmd_video_stop(video);
    return FALSE;
  }
  /* GstElement instances start with floating references. Own each element
     explicitly so playbin property replacement and error cleanup cannot
     invalidate our pointers. */
  gst_object_ref_sink(video->pipeline);
  gst_object_ref_sink(audio);
  gst_object_ref_sink(video_sink);

  gst_bin_add_many(GST_BIN(video_sink),queue,rate,scale,convert,video->capsfilter,
                   video->sink,NULL);
  if(!gst_element_link_many(queue,rate,scale,convert,video->capsfilter,video->sink,NULL)){
    g_set_error(error,G_FILE_ERROR,G_FILE_ERROR_FAILED,"cannot build optimized video sink");
    gst_object_unref(audio);gst_object_unref(video_sink);wmd_video_stop(video);return FALSE;}
  GstPad *pad=gst_element_get_static_pad(queue,"sink");
  gst_element_add_pad(video_sink,gst_ghost_pad_new("sink",pad));gst_object_unref(pad);
  gst_object_ref(video->sink);

  update_caps(video);
  g_object_set(video->sink,"max-buffers",1,"drop",TRUE,
               "sync",TRUE,"emit-signals",TRUE,NULL);
  g_signal_connect(video->sink,"new-sample",G_CALLBACK(new_sample),video);

  gchar *uri=g_filename_to_uri(path,NULL,error);
  if (!uri) {
    gst_object_unref(audio);gst_object_unref(video_sink);
    wmd_video_stop(video);
    return FALSE;
  }
  guint flags=0;g_object_get(video->pipeline,"flags",&flags,NULL);
  if(!g_strcmp0(video->hardware,"off"))flags|=0x1000;else flags&=~0x1000;
  if(!g_strcmp0(video->hardware,"on")&&!hardware_h264_available()){
    g_set_error(error,G_FILE_ERROR,G_FILE_ERROR_FAILED,
      "hardware decoding requested but no VA H.264 decoder is installed");
    g_free(uri);gst_object_unref(audio);gst_object_unref(video_sink);wmd_video_stop(video);return FALSE;}
  g_object_set(video->pipeline,"flags",flags,"uri",uri,"video-sink",video_sink,
               "audio-sink",audio,NULL);
  g_free(uri);
  gst_object_unref(audio);
  gst_object_unref(video_sink);

  wpd_renderer_begin_video(video->renderer,mode);
  GstBus *bus=gst_element_get_bus(video->pipeline);
  GstStateChangeReturn changed=gst_element_set_state(video->pipeline,GST_STATE_PLAYING);
  if (changed!=GST_STATE_CHANGE_FAILURE)
    changed=gst_element_get_state(video->pipeline,NULL,NULL,3*GST_SECOND);
  if (changed==GST_STATE_CHANGE_FAILURE) {
    GstMessage *message;
    GError *playback_error=NULL;
    gchar *missing=NULL;
    while ((message=gst_bus_pop(bus))) {
      if (!missing && gst_is_missing_plugin_message(message))
        missing=gst_missing_plugin_message_get_description(message);
      else if (!playback_error && GST_MESSAGE_TYPE(message)==GST_MESSAGE_ERROR)
        gst_message_parse_error(message,&playback_error,NULL);
      gst_message_unref(message);
    }
    if (missing)
      g_set_error(error,G_FILE_ERROR,G_FILE_ERROR_FAILED,
                  "missing GStreamer plug-in: %s",missing);
    else
      g_set_error(error,G_FILE_ERROR,G_FILE_ERROR_FAILED,"%s",
                  playback_error?playback_error->message:"GStreamer refused playback");
    g_free(missing);
    g_clear_error(&playback_error);
    gst_object_unref(bus);
    wmd_video_stop(video);
    return FALSE;
  }
  video->bus_watch=gst_bus_add_watch(bus,bus_message,video);
  gst_object_unref(bus);
  return TRUE;
}

void wmd_video_free(WmdVideo *video) {
  if (!video) return;
  wmd_video_stop(video);
  g_free(video->hardware);g_free(video);
}
