#pragma once

// The backend is C and its headers declare no C linkage of their own. GLib's
// headers do, and carry C++ templates that must not end up inside extern "C",
// so they are pulled in first.

extern "C" {
#include "gstreamer_backend.h"
#include "gstreamer_codec.h"
}
