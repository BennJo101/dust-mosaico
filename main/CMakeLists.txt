idf_component_register(
    SRCS "main.c" "dust_render.c" "dust_layout.c" "dust_warp.c" "dust_topo.c" "mosaic_net.c" "board_port.c"
         "dust_consensus.c" "dust_reliable.c" "dust_arb.c" "dust_adpcm.c" "dust_voice.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash esp_lcd esp_partition esp_driver_gpio esp-mosaico-bsp
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math)

# Dust's recorded lines (tools/make_clips.py writes clips/clips.bin) go into the "clips" partition
# when you flash, if you've made them. Without them Dust still works; he just can't speak offline.
set(clips_bin "${CMAKE_SOURCE_DIR}/clips/clips.bin")
if(EXISTS "${clips_bin}")
    esptool_py_flash_to_partition(flash "clips" "${clips_bin}")
endif()
