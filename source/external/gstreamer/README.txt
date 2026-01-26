GStreamer Backend for UnionPlayer
==================================

To enable GStreamer backend support:

1. Download GStreamer MSVC Runtime (1.0) from:
   https://gstreamer.freedesktop.org/download/

   Download both:
   - gstreamer-1.0-msvc-x86_64-X.XX.X.msi (runtime)
   - gstreamer-1.0-devel-msvc-x86_64-X.XX.X.msi (development)

2. Install to this directory structure or copy the required files:

   include/
     gst/gst.h
     gstreamer-1.0/...
     glib-2.0/...

   lib/
     gstreamer-1.0.lib
     gstbase-1.0.lib
     gstvideo-1.0.lib
     gstaudio-1.0.lib
     gstapp-1.0.lib
     gstpbutils-1.0.lib
     glib-2.0.lib
     gobject-2.0.lib
     gmodule-2.0.lib
     glib-2.0/include/glibconfig.h

   bin/
     gstreamer-1.0-0.dll
     gstbase-1.0-0.dll
     gstvideo-1.0-0.dll
     gstaudio-1.0-0.dll
     gstapp-1.0-0.dll
     gstpbutils-1.0-0.dll
     glib-2.0-0.dll
     gobject-2.0-0.dll
     gmodule-2.0-0.dll
     gio-2.0-0.dll
     intl-8.dll
     ffi-8.dll
     pcre2-8-0.dll

   plugins/
     gstcoreelements.dll    - filesrc, appsink, queue
     gstplayback.dll        - decodebin
     gstvideoconvert.dll    - videoconvert
     gstaudioconvert.dll    - audioconvert
     gstlibav.dll           - FFmpeg codecs (H.264, H.265, ProRes, DNxHD)
     gstd3d11.dll           - D3D11VA hardware acceleration

3. Configure CMake with:
   cmake -DWITH_GSTREAMER=ON ..

4. Build the project.

Notes:
- The gst-libav plugin provides FFmpeg codec support
- The gst-d3d11 plugin provides hardware acceleration on Windows
- Without GStreamer, the build falls back to direct FFmpeg decoding
