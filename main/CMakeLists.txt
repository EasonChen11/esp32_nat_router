idf_component_register(SRCS "http_server.c" "esp32_nat_router.c" "OLED.c"
                           
                    INCLUDE_DIRS ".")
set_source_files_properties(http_server.c
    PROPERTIES COMPILE_FLAGS
    -Wno-unused-function
)
