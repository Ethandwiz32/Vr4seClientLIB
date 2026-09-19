# ─────────────────────────────────────────────────────────────────────────────
# Android.mk — vr4seclient v3
# ─────────────────────────────────────────────────────────────────────────────
LOCAL_PATH := $(call my-dir)

# ── Dobby inline hook library (prebuilt) ────────────────────────────────────
# Dobby is the hook engine used by MonkiiGUI (confirmed in libMonkiiGUI.so:
#   "DobbyHook" string is present as an exported symbol).
# Download: https://github.com/jmpews/Dobby/releases
# Drop the AArch64 libdobby.a into ./libs/arm64-v8a/
include $(CLEAR_VARS)
LOCAL_MODULE           := dobby
LOCAL_SRC_FILES        := libs/arm64-v8a/libdobby.a
include $(PREBUILT_STATIC_LIBRARY)

# ── Main mod library ─────────────────────────────────────────────────────────
include $(CLEAR_VARS)

LOCAL_MODULE            := vr4seclient
LOCAL_SRC_FILES         := vr4seclient.cpp \
                           gui.cpp

LOCAL_C_INCLUDES        := $(LOCAL_PATH)

LOCAL_STATIC_LIBRARIES  := dobby

LOCAL_LDLIBS            := -llog \
                           -landroid \
                           -lm \
                           -ldl

# NOTE: We removed -lEGL -lGLESv3 — v3 uses Android Canvas, no GL at all.
# We removed -lGLESv3 to eliminate the crash from GL calls on foreign threads.

LOCAL_CPPFLAGS          := -std=c++17 \
                           -O2 \
                           -fvisibility=hidden \
                           -fstack-protector-strong \
                           -DANDROID \
                           -fexceptions

include $(BUILD_SHARED_LIBRARY)
