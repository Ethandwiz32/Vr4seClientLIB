# Android.mk — vr4seclient
# NDK r25c | AArch64 | API 29
# Build: cd <project_root> && ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk
# Output: libs/arm64-v8a/libvr4seclient.so

LOCAL_PATH := $(call my-dir)/..

include $(CLEAR_VARS)

LOCAL_MODULE            := vr4seclient
LOCAL_SRC_FILES         := src/vr4seclient.cpp \
                           src/gui.cpp

LOCAL_C_INCLUDES        := $(LOCAL_PATH)/include

# Standard Android libs — no third-party frameworks
LOCAL_LDLIBS            := -llog \
                           -landroid \
                           -lEGL \
                           -lGLESv3 \
                           -lm

# Hardening flags — strip symbols in release, keep them for debugging
LOCAL_CPPFLAGS          := -std=c++17 \
                           -O2 \
                           -fvisibility=hidden \
                           -fstack-protector-strong \
                           -Wall -Wextra \
                           -Wno-unused-parameter

# Hide all symbols by default — only JNI_OnLoad is visible (needed for dlopen)
LOCAL_CPPFLAGS          += -fvisibility=hidden
LOCAL_EXPORT_CPPFLAGS   += -fvisibility=hidden

include $(BUILD_SHARED_LIBRARY)
