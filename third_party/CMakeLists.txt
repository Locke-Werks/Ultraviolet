# r8brain-free-src is header-only: all code is inline, no source files to compile.
add_library(r8brain INTERFACE)
target_include_directories(r8brain INTERFACE r8brain-free-src)
