# Application.mk — vr4seclient
# Target only AArch64 (modern Android VR headsets are all arm64)
APP_ABI          := arm64-v8a
APP_PLATFORM     := android-29
APP_STL          := c++_static
APP_OPTIM        := release
APP_CPPFLAGS     := -std=c++17
