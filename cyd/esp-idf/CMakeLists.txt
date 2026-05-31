cmake_minimum_required(VERSION 3.16)

# Pull in the shared CYD component (ui, input, mcp_input, album_art, littlefs)
# that lives one level up so both this build and cyd/esp-idf-ha link the same
# code instead of hand-maintaining duplicate copies.
list(APPEND EXTRA_COMPONENT_DIRS ../components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(music_controller)

# JPEGDEC (bitbank2/jpegdec) has a few portability slips that the IDF
# strict-warning preset (-Wall -Wextra -Werror) won't accept on Xtensa:
#   - pointer-to-int casts through int64_t (32-bit target)
#   - left shift of a negative literal in IDCT scratch math
#   - unused legacy function (readFLASH) and an unused local variable
# Demote these warnings to non-fatal *for this component only* so we
# don't have to fork or vendor JPEGDEC.
idf_component_get_property(jpegdec_lib bitbank2__jpegdec COMPONENT_LIB)
if(jpegdec_lib)
    target_compile_options(${jpegdec_lib} PRIVATE
        -Wno-error=pointer-to-int-cast
        -Wno-error=shift-negative-value
        -Wno-error=unused-function
        -Wno-error=unused-variable
    )
endif()

# Force LV_USE_OBSERVER=1 on the LVGL component. LVGL 9.5.0 compiles
# lv_observer.c unconditionally but the types it uses (lv_subject_t,
# lv_observer_cb_t, ...) live in #if LV_USE_OBSERVER blocks in the
# headers. In a Kconfig build the type defaults to 0 unless an explicit
# CONFIG_LV_USE_OBSERVER=y reaches lv_conf_internal.h, which isn't
# happening reliably in our setup. Define the macro directly so the
# #ifndef LV_USE_OBSERVER guard short-circuits and the types are
# always declared.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
if(lvgl_lib)
    target_compile_definitions(${lvgl_lib} PUBLIC LV_USE_OBSERVER=1)
endif()
