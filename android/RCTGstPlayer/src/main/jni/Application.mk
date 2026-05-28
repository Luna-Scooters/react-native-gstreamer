APP_PLATFORM = 27
APP_ABI = armeabi-v7a armeabi arm64-v8a x86_64
APP_SUPPORT_FLEXIBLE_PAGE_SIZES := true
# Force 16KB-compatible ELF segment alignment for all shared libraries.
APP_LDFLAGS += -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384
#
