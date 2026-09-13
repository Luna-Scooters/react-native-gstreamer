# The GStreamer plugins owning the elements the pipelines in common/src
# instantiate -- the single source of truth for the plugin set. Platform
# specific plugins are appended by the if(APPLE)/elseif(ANDROID) branch below.
#
# On Apple it also builds GST_IOS_PLUGIN_LIST_DEFINE, the X-macro
# gst_ios_init.m expands into its declarations and register calls.

set(GSTREAMER_PLUGINS
    coreelements            # tee, queue, capsfilter, filesink
    rtsp rtpmanager rtp     # rtspsrc, rtpjitterbuffer, rtp{jpeg,h264}depay
    videoconvertscale videorate
    jpegformat jpeg         # jpegparse, jpegdec
    videoparsersbad         # h264parse
    isomp4                  # mp4mux
    openh264 libav)         # software encoder/decoder fallbacks

if(APPLE)
    list(APPEND GSTREAMER_PLUGINS applemedia)
    list(APPEND GSTREAMER_PLUGINS vulkan)      # vulkanupload, vulkancolorconvert, vulkansink
elseif(ANDROID)
    list(APPEND GSTREAMER_PLUGINS androidmedia)
    list(APPEND GSTREAMER_PLUGINS opengl)      # glimagesink
    list(APPEND GSTREAMER_PLUGINS autoconvert) # autovideoconvert
endif()

option(GST_DEBUG_PIPELINE "Include plugins for the isDebugging test pipeline" OFF)
if(GST_DEBUG_PIPELINE)
    list(APPEND GSTREAMER_PLUGINS videotestsrc opengl)
endif()

list(REMOVE_DUPLICATES GSTREAMER_PLUGINS)

if(CMAKE_SCRIPT_MODE_FILE)
    list(TRANSFORM GSTREAMER_PLUGINS PREPEND "F(")
    list(TRANSFORM GSTREAMER_PLUGINS APPEND ")")
    list(JOIN GSTREAMER_PLUGINS "" GSTREAMER_PLUGINS)
    execute_process(COMMAND ${CMAKE_COMMAND} -E echo "${GSTREAMER_PLUGINS}")
endif()
