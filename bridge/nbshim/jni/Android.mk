LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := modavd_bridge
LOCAL_SRC_FILES := native_bridge_shim.cc
# DO NOT add -ftls-model=initial-exec here, and do not use thread_local anywhere in
# this module. libnativebridge dlopen()s this library, and a dlopened library
# carrying initial-exec TLS relocations is rejected by the Bionic linker —
#   nativebridge: Failed to load native bridge implementation: dlopen failed:
#   TLS symbol "(null)" in dlopened "/system/lib64/libmodavd_bridge.so"
# — so the whole native bridge fails to load and no arm64 app starts at all.
# Check the output with:  llvm-readelf -r libmodavd_bridge.so | grep TPOFF64
# (must print nothing).
LOCAL_CPPFLAGS  := -std=c++17 -fvisibility=hidden -O2 -Wall -Wextra -fno-exceptions -fno-rtti
LOCAL_LDLIBS    := -llog -ldl
include $(BUILD_SHARED_LIBRARY)
