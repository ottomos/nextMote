#set(COMPONENT_SRCS "main.c")
#set(COMPONENT_ADD_INCLUDEDIRS "")

#register_component()

idf_component_register(SRCS "wiimote_test.c" "wiimote_descriptor.c" "wiimote_identity.c" "wiimote_web.c" "ir_camera.c"
                    PRIV_REQUIRES bt esp_http_server esp_netif esp_wifi esp_driver_gpio esp_driver_i2c nvs_flash
                    INCLUDE_DIRS ".")
