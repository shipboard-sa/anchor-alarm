# Purpose: Top-level ESP-IDF project file for the anchor box firmware (clean-room build, ANC-01).
# Version: v2 (2026-10-09) - v2: LVGL's 64 KB object pool placed in PSRAM (ANQ-33). v1 (2026-10-02)
# Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
# Architecture: <your-project-dir>; the only component is main/ (+ lvgl from the component registry).
# Invocation: idf.py build (from <your-project-dir>, ESP-IDF v6.0.3 environment) - see README.md.
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(anchor_box)

# ANQ-33 (2026-10-09): LVGL's built-in heap is a 64 KB static array (lv_mem_core_builtin.c work_mem_int) that sat in
# internal DRAM. LVGL places it with the LV_ATTRIBUTE_LARGE_RAM_ARRAY macro; ESP-IDF's EXT_RAM_BSS_ATTR (esp_attr.h,
# needs CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY in sdkconfig.defaults) puts a .bss array in PSRAM. LVGL is built
# with LV_OS_NONE, so esp_attr.h is force-included for the lvgl library only. Check: build/anchor_box.map shows
# .ext_ram.bss for work_mem_int, and `idf.py size` DIRAM .bss drops by ~64 KB.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_definitions(${lvgl_lib} PRIVATE LV_ATTRIBUTE_LARGE_RAM_ARRAY=EXT_RAM_BSS_ATTR)
target_compile_options(${lvgl_lib} PRIVATE -include esp_attr.h)
