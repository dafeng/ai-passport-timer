idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "countdown_timer.c"
         "countdown_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hw_support esp_timer
)
