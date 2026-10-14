add_executable(minimal)

pico_generate_pio_header(minimal ${CMAKE_CURRENT_LIST_DIR}/minimal.pio)

# Enable this for PAL
if (0)
    target_compile_definitions(minimal PRIVATE
        PAL=1
        PLL_SYS_REFDIV=2
        PLL_SYS_VCO_FREQ_HZ=1206000000
        PLL_SYS_POSTDIV1=4
        PLL_SYS_POSTDIV2=2
        SYS_CLK_HZ=150750000
    )
endif()

target_sources(minimal PRIVATE minimal.c)

# pull in common dependencies
target_link_libraries(minimal pico_stdlib hardware_pio hardware_dma)

# create map/bin/hex file etc.
pico_add_extra_outputs(minimal)

