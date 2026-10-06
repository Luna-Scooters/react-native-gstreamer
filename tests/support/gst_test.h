#pragma once

// What every suite shares: main() (gst_test.cpp) and RAII for GStreamer

#include <gst/gst.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace std::chrono_literals;

// ---- ownership ----

struct GstObjectUnref {
    void operator()(gpointer object) const { if (object) gst_object_unref(object); }
};
template <typename T> using GstPtr = std::unique_ptr<T, GstObjectUnref>;
