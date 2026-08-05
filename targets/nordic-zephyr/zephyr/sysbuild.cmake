# Sysbuild runs before the app CMakeLists; BOARD_ROOT must be set here
# so out-of-tree boards under ../boards/ are visible.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
