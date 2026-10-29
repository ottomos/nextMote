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
                    "sensor_i2c_bus.c"
                    "ir_camera.c"
                    "ir_camera_service.c"
                    "imu_service.c"
                    PRIV_REQUIRES bno085 bt esp_http_server esp_netif esp_wifi esp_driver_gpio esp_driver_i2c nvs_flash
                    INCLUDE_DIRS ".")

# The registry BNO085 component uses legacy driver include paths; IDF 6 split these into driver components.
idf_component_get_property(BNO085_COMPONENT_LIB rinku404__bno085 COMPONENT_LIB)
idf_component_get_property(ESP_DRIVER_I2C_DIR esp_driver_i2c COMPONENT_DIR)
idf_component_get_property(ESP_DRIVER_GPIO_DIR esp_driver_gpio COMPONENT_DIR)
target_include_directories(${BNO085_COMPONENT_LIB} PRIVATE
                           "${ESP_DRIVER_I2C_DIR}/include"
                           "${ESP_DRIVER_GPIO_DIR}/include")
