# Shared Pico headers/helpers (header-only for now).
add_library(rv_common INTERFACE)
target_include_directories(rv_common INTERFACE ${CMAKE_CURRENT_LIST_DIR})
