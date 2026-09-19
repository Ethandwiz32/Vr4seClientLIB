LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE            := vr4seclient
LOCAL_SRC_FILES         := vr4seclient.cpp \
                           gui.cpp

LOCAL_C_INCLUDES        := $(LOCAL_PATH)

LOCAL_LDLIBS            := -llog \
                           -landroid \
                           -lEGL \
                           -lGLESv3 \
                           -lm

LOCAL_CPPFLAGS          := -std=c++17 \
                           -O2 \
                           -fvisibility=hidden \
                           -fstack-protector-strong

include $(BUILD_SHARED_LIBRARY)
