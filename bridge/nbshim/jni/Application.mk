APP_ABI      := x86_64
APP_PLATFORM := android-30
# No STL is used or needed: the shim touches only libc, libdl and liblog. Keeping
# it at "none" also guarantees no libc++ dependency in a library that sits in the
# zygote boot path.
APP_STL      := none
