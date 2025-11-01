#ifndef GSTREAMER_PLAYER_H
#define GSTREAMER_PLAYER_H

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/gl/gl.h>
#include <gst/gl/x11/gstgldisplay_x11.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <thread>

class GStreamerPlayer {

private:
    GstElement *pipeline;
    GstElement *source;
    GstElement *sink;
    GstBus *bus;
    std::thread myThread;
    
    // GPU Memory handling
    GstGLDisplay *gl_display;
    GstGLContext *gl_context;
    
    // Godot Integration
    bool is_playing;
    bool use_vulkan;
    bool has_new_frame_flag;
    
    int last_frame_width = 0;
    int last_frame_height = 0;
    GstVideoFormat last_frame_format = GST_VIDEO_FORMAT_UNKNOWN;
    
    // Callbacks
    static GstFlowReturn new_sample_callback(GstAppSink *sink, gpointer user_data);
    static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer user_data);
    static void pad_added_callback(GstElement* src, GstPad* new_pad, gpointer user_data);
    
    // Internal methods
    bool setup_pipeline(const char* &uri);
    void cleanup();
    void runMainLoop();
    void setup_context();

protected:
    static void _bind_methods();

public:
    GMainLoop *main_loop;

    GStreamerPlayer();
    ~GStreamerPlayer();
    
    // Public API
    void create_window(int width, int height);

    bool open_stream(const char* &uri);
    void play();
    void pause();
    void stop();
    void close();
    
    bool is_stream_playing() const;
    void apply_last_frame();

    // GPU Memory specific
    bool has_new_frame() const;
};

#endif // GSTREAMER_PLAYER_H
