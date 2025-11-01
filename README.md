# gstreamer-manual-opengl-rendering
Example how to manual render opengl textures received by gstreamer in a X11 window

Sources
+ https://gstreamer-devel.narkive.com/hCRNBfw0/opengl-texture-via-gstglupload-so-close-proper-post
+ https://github.com/Swap-File/gst-context-share/tree/master
+ (To my shame 😔) ChatGPT

Compiling:  
``g++ gstreamer_test.cpp -o gstreamer_test $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0 gobject-2.0 glib-2.0 gstreamer-gl-1.0 gstreamer-app-1.0 gstreamer-video-1.0) -lGLX -lGL``
