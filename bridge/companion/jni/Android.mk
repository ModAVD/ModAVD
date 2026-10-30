LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := modavd_gate
LOCAL_SRC_FILES := native_to_guest_companion.cc
# DO NOT add -ftls-model=initial-exec here. It was tried and it bricks the device's
# arm64 support outright: libnativebridge dlopen()s the translator, we are its
# DT_NEEDED, and a dlopened library carrying initial-exec TLS relocations is
# rejected by the linker —
#   nativebridge: Failed to load native bridge implementation: dlopen failed:
#   TLS symbol "(null)" in dlopened "/system/lib64/libmodavd_gate.so"
# — so the whole native bridge fails to load and no arm64 app starts at all.
# The signal handler therefore uses a tid table instead of thread-local storage.
LOCAL_CPPFLAGS  := -std=c++17 -fvisibility=hidden -O2 -Wall -Wextra -fno-exceptions -fno-rtti
LOCAL_LDLIBS    := -llog
include $(BUILD_SHARED_LIBRARY)
