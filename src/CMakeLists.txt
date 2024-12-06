add_executable(proyectofinal
    main.c 
    ssd1306.c)

    target_include_directories(proyectofinal PRIVATE
    ${CMAKE_CURRENT_LIST_DIR})

    target_compile_definitions(proyectofinal PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
    )

    target_link_libraries(proyectofinal
        pico_stdlib
        FreeRTOS-Kernel-Heap4
        hardware_i2c
        pico_cyw43_arch_lwip_threadsafe_background
        pico_lwip_mqtt
    )
    pico_add_extra_outputs(proyectofinal)
    pico_enable_stdio_usb(proyectofinal 1)
    pico_enable_stdio_uart(proyectofinal 0)
