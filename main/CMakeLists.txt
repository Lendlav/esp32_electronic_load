idf_component_register(
    SRCS
         # Composition root: wires adapters to the application.
         "app_main.c"
         # Hardware-independent rules and application workflows.
         "domain/load_controller.c"
         "domain/pi_controller.c"
         "application/load_app.c"
         # ESP-IDF implementations of application ports.
         "adapters/ina219.c"
         "adapters/pwm_output.c"
         "adapters/buttons.c"
         "adapters/settings_storage.c"
         "adapters/display.c"
         "adapters/web_server.c"
    INCLUDE_DIRS "." "domain" "application" "adapters" "platform"
    EMBED_TXTFILES "web_page.html"
    PRIV_REQUIRES driver esp_lcd esp_lcd_sh1107 esp_timer esp_lvgl_port lvgl log
                  esp_wifi esp_event esp_netif esp_http_server nvs_flash
)
