#set(COMPONENT_SRCS "main.c")
#set(COMPONENT_ADD_INCLUDEDIRS "")

#register_component()

idf_component_register(SRCS
                    "wiimote_app.c"
                    "wiimote_state.c"
                    "wiimote_reports.c"
                    "wiimote_commands.c"
                    "wiimote_memory.c"
                    "wiimote_controls.c"
                    "wiimote_protocol.c"
                    "wiimote_descriptor.c"
                    "wiimote_identity.c"
                    "wiimote_web.c"
                    "ir_camera.c"
                    "ir_camera_service.c"
                    PRIV_REQUIRES bt esp_http_server esp_netif esp_wifi esp_driver_gpio esp_driver_i2c nvs_flash
                    INCLUDE_DIRS ".")
