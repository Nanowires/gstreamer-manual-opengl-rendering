#include "gstreamer_test.h"
#include <GL/glx.h>
#include <thread>
#include <stdio.h>
#include <math.h>
#include <mutex>

static Display *disp = nullptr;
static Window win;

static GLXContext ctx;
static GstContext *x11context = nullptr;
static GstContext *ctxcontext = nullptr;

static volatile bool is_running = true;
static GstSample *latest_sample = nullptr;
static std::mutex sample_mutex;

GStreamerPlayer::GStreamerPlayer() {
    pipeline = nullptr;
    source = nullptr;
    sink = nullptr;
    bus = nullptr;
    gl_display = nullptr;
    gl_context = nullptr;
    main_loop = nullptr;
    is_playing = false;
    has_new_frame_flag = false;
    
    // Initialize GStreamer
    gst_init(nullptr, nullptr);
}

GStreamerPlayer::~GStreamerPlayer() {
    cleanup();
    gst_deinit();
}

bool GStreamerPlayer::open_stream(const char* &uri) {
    return setup_pipeline(uri);// && setup_gl_context();
}

static GstBusSyncReply my_create_window(GstBus *bus, GstMessage *message, GstPipeline *pipeline)
{
    // ignore anything but 'prepare-window-handle' element messages
    if (!gst_is_video_overlay_prepare_window_handle_message(message))
        return GST_BUS_PASS;

    Display *disp = XOpenDisplay(NULL);
    Window win = XCreateSimpleWindow(disp, RootWindow(disp, 0), 0, 0, 1920, 1080, 0, 0, 0);

    XSetWindowBackgroundPixmap(disp, win, None);

    XMapRaised(disp, win);

    XSync(disp, FALSE);

    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(message)), win);

    gst_message_unref(message);

    return GST_BUS_DROP;
}

bool GStreamerPlayer::setup_pipeline(const char* &uri) {
    cleanup();
    
    main_loop = g_main_loop_new(NULL, FALSE);

    // Create pipeline
    pipeline = gst_pipeline_new("video-pipeline");
    
    // Create elements based on URI
    if (strncmp(uri, "rtsp://", strlen("rtsp://")) == 0) {
        source = gst_element_factory_make("rtspsrc", "source");
    } else if (strncmp(uri, "udp://", strlen("udp://")) == 0) {
        source = gst_element_factory_make("udpsrc", "source");
    } else if ((strncmp(uri, "http://", strlen("http://")) == 0) || (strncmp(uri, "https://", strlen("https://")) == 0)) {
        source = gst_element_factory_make("souphttpsrc", "source");
    } else {
        source = gst_element_factory_make("filesrc", "source");
    }
    
    // Pipeline-Elements
    GstElement *decodebin = gst_element_factory_make("decodebin", "decoder");
    GstElement *queue = gst_element_factory_make("queue", "queue");
    GstElement *videoscale = gst_element_factory_make("videoscale", "videoscale");
    GstElement *capsfilter = gst_element_factory_make("capsfilter", "capsfilter");
    GstElement *glupload = gst_element_factory_make("glupload", "glupload");
    GstElement *glcolorconvert = gst_element_factory_make("glcolorconvert", "glcolorconvert");
    // Optional: Force usage of GPU memory
    GstElement *glfilterapp = gst_element_factory_make("glfilterapp", "glfilterapp");
    GstElement *appsink = gst_element_factory_make("appsink", "appsink");

    if (!pipeline || !source || !decodebin || !queue || !videoscale || !capsfilter || !glupload || !glcolorconvert || !glfilterapp || !appsink) {
        printf("Could not create pipeline elements\n");
        return false;
    }

    GstElement *audio_queue = gst_element_factory_make("queue", "audio_queue");
    GstElement *audio_convert = gst_element_factory_make("audioconvert", "audio_convert");
    GstElement *audio_resample = gst_element_factory_make("audioresample", "audio_resample");
    GstElement *audio_sink = gst_element_factory_make("autoaudiosink", "audio_sink");

    if (!audio_queue || !audio_convert || !audio_resample || !audio_sink) {
        printf("Could not create audio elements\n");
        return false;
    }

    // Set URI
    g_object_set(G_OBJECT(source), "location", uri, nullptr);

    // Configure scaling caps
    GstCaps *scale_caps = gst_caps_from_string("video/x-raw,width=1920,height=1080,force-aspect-ratio=false");
    g_object_set(capsfilter,
                 "caps", scale_caps,
                 nullptr);
    gst_caps_unref(scale_caps);
    
    // Configure appsink for video capture
    GstCaps *caps = gst_caps_from_string("video/x-raw(memory:GLMemory), format=RGBA");
    g_object_set(appsink,
                 "emit-signals", TRUE,
                 "sync", TRUE,
                 "caps", caps,
                 nullptr);
    gst_caps_unref(caps);
    
    g_signal_connect(appsink, "new-sample", G_CALLBACK(new_sample_callback), this);
    
    // Add elements to pipeline
    gst_bin_add_many(GST_BIN(pipeline), source, decodebin, queue, videoscale, capsfilter, glupload, glcolorconvert, glfilterapp, appsink, NULL);

    gst_bin_add_many(GST_BIN(pipeline), audio_queue, audio_convert, audio_resample, audio_sink, NULL);
    
    // Link source to decodebin
    if (!gst_element_link(source, decodebin)) {
        printf("Failed to link source and decodebin\n");
        return false;
    }

    // Link conversion elements
    if (!gst_element_link_many(queue, videoscale, capsfilter, glupload, glcolorconvert, glfilterapp, appsink, NULL)) {
        printf("Failed to link elements\n");
        return false;
    }
    if (!gst_element_link_many(audio_queue, audio_convert, audio_resample, audio_sink, NULL)) {
        printf("Failed to link audio elements\n");
        return false;
    }
    
    g_signal_connect(decodebin, "pad-added", G_CALLBACK(pad_added_callback), this);

    // Setup bus
    bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, this);

    return true;
}

void GStreamerPlayer::runMainLoop()
{
    g_main_loop_run(this->main_loop);
}

void GStreamerPlayer::play() {
    if (pipeline) {
        myThread = std::thread(&GStreamerPlayer::runMainLoop, this);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        is_playing = true;
    }
}

void GStreamerPlayer::pause() {
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        is_playing = false;
    }
}

void GStreamerPlayer::stop() {
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        is_playing = false;
    }
}

void GStreamerPlayer::close() {
    cleanup();
}

void GStreamerPlayer::cleanup() {
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = nullptr;
    }
    
    if (bus) {
        gst_object_unref(bus);
        bus = nullptr;
    }
    
    if (gl_context) {
        gst_object_unref(gl_context);
        gl_context = nullptr;
    }
    
    if (gl_display) {
        gst_object_unref(gl_display);
        gl_display = nullptr;
    }
    if (main_loop) {
        g_main_loop_quit(main_loop);
        g_main_loop_unref(main_loop);
        main_loop = nullptr;
        myThread.join();
    }
    
    is_playing = false;
}

bool GStreamerPlayer::is_stream_playing() const {
    return is_playing;
}

GstFlowReturn GStreamerPlayer::new_sample_callback(GstAppSink *sink, gpointer user_data) {
    GStreamerPlayer *player = static_cast<GStreamerPlayer*>(user_data);
    
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample) {
        // player->render_frame(sample);
        // gst_sample_unref(sample);
        {
            std::lock_guard<std::mutex> lock(sample_mutex);
            if (latest_sample)  {
                gst_sample_unref(latest_sample);
            }
            latest_sample = sample;
        }
    }
    
    return GST_FLOW_OK;
}

void GStreamerPlayer::pad_added_callback(GstElement* src, GstPad* new_pad, gpointer user_data)
{
    GStreamerPlayer* player = static_cast<GStreamerPlayer*>(user_data);

    GstCaps* new_pad_caps = gst_pad_get_current_caps(new_pad);
    if (!new_pad_caps) {
        new_pad_caps = gst_pad_query_caps(new_pad, nullptr);
    }

    GstStructure* new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    const gchar* new_pad_type = gst_structure_get_name(new_pad_struct);

    if (g_str_has_prefix(new_pad_type, "video/x-raw")) {
        printf("Linking video pad...\n");
        GstElement* video_queue = gst_bin_get_by_name(GST_BIN(player->pipeline), "queue");
        GstPad* sink_pad = gst_element_get_static_pad(video_queue, "sink");
        if (!gst_pad_is_linked(sink_pad)) {
            if (gst_pad_link(new_pad, sink_pad) == GST_PAD_LINK_OK) {
                printf("Video pad linked successfully\n");
            } else {
                printf("Failed to link video pad\n");
            }
        }
        gst_object_unref(sink_pad);
        gst_object_unref(video_queue);
    } else if (g_str_has_prefix(new_pad_type, "audio/x-raw")) {
        printf("Linking audio pad...\n");
        GstElement* audio_queue = gst_bin_get_by_name(GST_BIN(player->pipeline), "audio_queue");
        GstPad* sink_pad = gst_element_get_static_pad(audio_queue, "sink");
        if (!gst_pad_is_linked(sink_pad)) {
            if (gst_pad_link(new_pad, sink_pad) == GST_PAD_LINK_OK) {
                printf("Audio pad linked successfully\n");
            } else {
                printf("Failed to link audio pad\n");
            }
        }
        gst_object_unref(sink_pad);
        gst_object_unref(audio_queue);
    } else {
        printf("Ignoring pad of type: %s\n", new_pad_type);
    }

    gst_caps_unref(new_pad_caps);
}

gboolean GStreamerPlayer::bus_callback(GstBus *bus, GstMessage *message, gpointer user_data) {
    GStreamerPlayer *player = static_cast<GStreamerPlayer*>(user_data);
    
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *error;
            gchar *debug_info;
            gst_message_parse_error(message, &error, &debug_info);
            printf("GStreamer Error: %s\n", error->message);
            if (debug_info) {
                printf("Debug info: %s\n", debug_info);
            }
            g_clear_error(&error);
            g_free(debug_info);
            player->is_playing = false;
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *error;
            gchar *debug_info;
            gst_message_parse_warning(message, &error, &debug_info);
            printf("GStreamer Warning: %s\n", error->message);
            if (debug_info) {
                printf("Debug info: %s\n", debug_info);
            }
            g_clear_error(&error);
            g_free(debug_info);
            break;
        }
        case GST_MESSAGE_EOS:
            printf("End of stream reached");
            player->is_playing = false;
            break;
        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(message, &old_state, &new_state, &pending_state);
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(player->pipeline)) {
                printf("Pipeline state changed from %s to %s\n", gst_element_state_get_name(old_state), gst_element_state_get_name(new_state));
            }
            break;
        }
        case GST_MESSAGE_STREAM_STATUS: {
            GstStreamStatusType type;
            GstElement* owner;
            gst_message_parse_stream_status(message, &type, &owner);
            printf("Stream status: %d\n", type);
            break;
        }
        case GST_MESSAGE_NEED_CONTEXT: // THIS IS THE IMPORTANT PART
        {
            printf("Need context message received\n");
            const gchar *context_type;
            gst_message_parse_context_type(message, &context_type);
            if (g_strcmp0(context_type, "gst.gl.app_context") == 0)
            {
                printf("OpenGL Context Request Intercepted! %s\n", context_type);
                gst_element_set_context(GST_ELEMENT(message->src), ctxcontext);
            }
            if (g_strcmp0(context_type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0)
            {
                printf("X11 Display Request Intercepted! %s\n", context_type);
                gst_element_set_context(GST_ELEMENT(message->src), x11context);
            }

            break;
        }
        case GST_MESSAGE_CLOCK_LOST: {
            printf("Clock lost, selecting new clock");
            break;
        }
        default:
            break;
    }
    
    return TRUE;
}

static void create_window(int width, int height)
{
    disp = XOpenDisplay(NULL);
	XSetWindowAttributes attr;

    int attribs[64];
	int i = 0;

    /* Singleton attributes. */
	attribs[i++] = GLX_RGBA;
	attribs[i++] = GLX_DOUBLEBUFFER;

	/* Key/value attributes. */
	attribs[i++] = GLX_RED_SIZE;
	attribs[i++] = 1;
	attribs[i++] = GLX_GREEN_SIZE;
	attribs[i++] = 1;
	attribs[i++] = GLX_BLUE_SIZE;
	attribs[i++] = 1;
	attribs[i++] = GLX_DEPTH_SIZE;
	attribs[i++] = 1;
	attribs[i++] = None;

	//get ctxcontext ready for handing off to elements in the callback
    int scrnum = DefaultScreen( disp );
	Window root = RootWindow( disp, scrnum );

    XVisualInfo *visinfo = glXChooseVisual(disp, scrnum, attribs);

	/* window attributes */
	attr.background_pixel = 0;
	attr.border_pixel = 0;
	attr.colormap = XCreateColormap( disp, root, visinfo->visual, AllocNone);
	attr.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask;
	/* XXX this is a bad way to get a borderless window! */
	unsigned long mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;

	win = XCreateWindow( disp, root, 0, 0, width, height, 0, visinfo->depth, InputOutput, visinfo->visual, mask, &attr );

	GstGLDisplay * gl_display = GST_GL_DISPLAY (gst_gl_display_x11_new_with_display (disp));		
	x11context = gst_context_new (GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
	gst_context_set_gl_display (x11context, gl_display);

    ctx = glXCreateContext( disp, visinfo, NULL, True );
	GstGLContext *gl_context = gst_gl_context_new_wrapped ( gl_display, (guintptr) ctx, GST_GL_PLATFORM_GLX,GST_GL_API_OPENGL); //ctx is the glx OpenGL context
	ctxcontext = gst_context_new ("gst.gl.app_context", TRUE);
	gst_structure_set (gst_context_writable_structure (ctxcontext), "context", GST_TYPE_GL_CONTEXT, gl_context, NULL);
	XMapWindow(disp, win);
	glXMakeCurrent(disp, win, ctx);
	XFree(visinfo);
}

static void render_frame(GstSample* sample) {
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    GstVideoFrame v_frame;
    GstVideoInfo v_info;

    if (!glXMakeCurrent(disp, win, ctx)) {
        printf("glXMakeCurrent FAILED\n");
        return;
    }

    GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
    if (!gst_is_gl_memory(mem)) {
        printf("Buffer memory is not GLMemory\n");
    }

    GstGLMemory *gl_mem = GST_GL_MEMORY_CAST(mem);
    GLuint tex = gst_gl_memory_get_texture_id(gl_mem);

	// Start with a clear screen
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_TEXTURE_2D);
	
	
	glPushMatrix();
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE,GL_REPLACE);
	glBindTexture (GL_TEXTURE_2D, tex); 

    // --- 6) draw full-screen quad ---
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f, -1.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f,  1.0f);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f,  1.0f);
    glEnd();

    // --- 7) cleanup minimal ---
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);

    // Swap (falls du hier swappen willst; alternativ in deinem main render thread)
    glXSwapBuffers(disp, win);
    
}

void window_thread()
{
    XEvent event;
    Atom del = XInternAtom(disp, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(disp, win, &del, 1);

    while (event.type != ClientMessage && event.xclient.data.l[0] != del) {
        XNextEvent(disp, &event);
    }
    printf("Window closed\n");
    is_running = false;
}

int main() { 
    GStreamerPlayer player;
    const char* uri = "REPLLACE_ME_WITH_YOUR_STREAM_URL";

    create_window(1920, 1080);

    if (!player.open_stream(uri)) {
        printf("Failed to open stream\n");
        return -1;
    }
    player.play();
    std::thread t1(window_thread);
    // Rendering must happen in the main thread, as the context was created there
    while (is_running) {
        GstSample *sample = nullptr;
        {
            std::lock_guard<std::mutex> lock(sample_mutex);
            if (latest_sample) {
                sample = gst_sample_ref(latest_sample);
            }
        }

        if (sample) {
            render_frame(sample);
            gst_sample_unref(sample);
        }
    }
    
    player.stop();
    player.close();
    t1.join();
    
    return 0;
}