add_executable(sync_burst_pio)

pico_generate_pio_header(sync_burst_pio ${CMAKE_CURRENT_LIST_DIR}/ntsc.pio)
pico_generate_pio_header(sync_burst_pio ${CMAKE_CURRENT_LIST_DIR}/pal.pio)

# Enable this for PAL
if (0)
    target_compile_definitions(sync_burst_pio PRIVATE
        PAL=1
        PLL_SYS_REFDIV=2
        PLL_SYS_VCO_FREQ_HZ=1206000000
        PLL_SYS_POSTDIV1=4
        PLL_SYS_POSTDIV2=2
        SYS_CLK_HZ=150750000
    )
endif()

target_sources(sync_burst_pio PRIVATE sync_burst_pio.c ccube_demo.c ccube_demo.h simple_bold_font_8x8_le1bit.h)

# pull in common dependencies
target_link_libraries(sync_burst_pio pico_stdlib hardware_pio hardware_dma)

# create map/bin/hex file etc.
pico_add_extra_outputs(sync_burst_pio)

