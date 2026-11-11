# The portable engine in ../../../../engine, built as an ESP-IDF component.
#
# Recreated: this directory was never committed, because the repo's
# .gitignore had a bare `needle/` (meant for the upstream package cloned at
# the root) that also matched this path. The ignore rule is now `/needle/`.
set(ND_ENGINE "${CMAKE_CURRENT_LIST_DIR}/../../../../engine")
file(GLOB ND_SRC "${ND_ENGINE}/src/*.c")

# The P4 int8 dot kernel is hand-written assembly; the S3 path is inline asm in
# nd_quant.c and the host build uses the scalar fallback, so only the P4 build
# compiles this file.
if(CONFIG_IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32p4")
    list(APPEND ND_SRC "${ND_ENGINE}/src/nd_p4_dot.S")
endif()

idf_component_register(SRCS ${ND_SRC}
                       INCLUDE_DIRS "${ND_ENGINE}/include"
                       REQUIRES esp_timer heap esp_common)

# Per-phase timings at boot (README: "Want a timing breakdown?"). Off by
# default because the timers cost a little speed.
# target_compile_definitions(${COMPONENT_LIB} PUBLIC ND_PROFILE)
