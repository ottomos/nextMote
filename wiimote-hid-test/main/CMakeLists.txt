#set(COMPONENT_SRCS "main.c")
#set(COMPONENT_ADD_INCLUDEDIRS "")

#register_component()

idf_component_register(SRCS
                    "wiimote_app.c"
                    "wiimote_state.c"
                    "wiimote_reports.c"
                    "wiimote_accel.c"
                    "wiimote_commands.c"
                    "wiimote_memory.c"
                    "wiimote_controls.c"
                    "wiimote_protocol.c"
                    "wiimote_descriptor.c"
                    "wiimote_identity.c"
                    "wiimote_discovery.c"
                    "wiimote_inquiry_policy.c"
                    "wiimote_sdp.c"
                    "wiimote_sdp_values.c"
                    "wiimote_web.c"
                    "sensor_i2c_bus.c"
                    "ir_camera.c"
                    "ir_camera_service.c"
                    "imu_service.c"
                    PRIV_REQUIRES bno085 bt esp_http_server esp_netif esp_wifi esp_timer esp_driver_gpio esp_driver_i2c nvs_flash
                    INCLUDE_DIRS ".")

option(WIIMOTE_DIAGNOSTIC_GIAC_ONLY "Hold GIAC during Limited SYNC for a bounded timing probe" OFF)
option(WIIMOTE_UPSTREAM_DISCOVERY_BASELINE "Use upstream-style continuous General discovery when idle" OFF)
option(WIIMOTE_DIAGNOSTIC_RADIO_ONLY "Disable Wi-Fi startup for a bounded upstream radio comparison" OFF)
if(WIIMOTE_DIAGNOSTIC_RADIO_ONLY AND NOT WIIMOTE_UPSTREAM_DISCOVERY_BASELINE)
    message(FATAL_ERROR "Radio-only diagnostic requires the continuous upstream discovery baseline")
endif()
if(WIIMOTE_DIAGNOSTIC_GIAC_ONLY AND WIIMOTE_UPSTREAM_DISCOVERY_BASELINE)
    message(FATAL_ERROR "Choose either the upstream discovery baseline or the static-IAC diagnostic")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           WIIMOTE_DIAGNOSTIC_GIAC_ONLY=$<BOOL:${WIIMOTE_DIAGNOSTIC_GIAC_ONLY}>
                           WIIMOTE_UPSTREAM_DISCOVERY_BASELINE=$<BOOL:${WIIMOTE_UPSTREAM_DISCOVERY_BASELINE}>
                           WIIMOTE_DIAGNOSTIC_RADIO_ONLY=$<BOOL:${WIIMOTE_DIAGNOSTIC_RADIO_ONLY}>)
if(DEFINED NEXTMOTE_SEND_IMU_TO_HID)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
                               NEXTMOTE_SEND_IMU_TO_HID=$<BOOL:${NEXTMOTE_SEND_IMU_TO_HID}>)
endif()

idf_component_get_property(BLUEDROID_BT_COMPONENT_DIR bt COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE
                           "${BLUEDROID_BT_COMPONENT_DIR}/common/include"
                           "${BLUEDROID_BT_COMPONENT_DIR}/porting/mem"
                           "${BLUEDROID_BT_COMPONENT_DIR}/host/bluedroid/hci/include"
                           "${BLUEDROID_BT_COMPONENT_DIR}/host/bluedroid/stack/include"
                           "${BLUEDROID_BT_COMPONENT_DIR}/host/bluedroid/common/include"
                           "${BLUEDROID_BT_COMPONENT_DIR}/host/bluedroid/osi/include")
target_link_options(${COMPONENT_LIB} INTERFACE
                    "-Wl,--wrap=btsnd_hcic_write_cur_iac_lap"
                    "-Wl,--wrap=HID_DevAddRecord")

if(CONFIG_EXAMPLE_BT_DISCOVERY_TRACE)
    target_sources(${COMPONENT_LIB} PRIVATE
                   "wiimote_bt_trace.c" "wiimote_bt_trace_packet.c"
                   "wiimote_bond_json.c")
    target_link_options(${COMPONENT_LIB} INTERFACE
                        "-Wl,--wrap=hci_host_send_packet"
                        "-Wl,--wrap=hci_host_register_callback")
endif()

# Sample inquiry scans more frequently while preserving the observed R1 page scan.
set(WIIMOTE_BT_INQUIRY_SCAN_WINDOW 0x0012)
set(WIIMOTE_BT_INQUIRY_SCAN_INTERVAL 0x0400)
set(WIIMOTE_BT_PAGE_SCAN_WINDOW 0x0012)
set(WIIMOTE_BT_PAGE_SCAN_INTERVAL 0x0800)
idf_component_get_property(BLUEDROID_BT_COMPONENT_LIB bt COMPONENT_LIB)
target_compile_definitions(${BLUEDROID_BT_COMPONENT_LIB} PRIVATE
                           BTM_DEFAULT_DISC_WINDOW=${WIIMOTE_BT_INQUIRY_SCAN_WINDOW}
                           BTM_DEFAULT_DISC_INTERVAL=${WIIMOTE_BT_INQUIRY_SCAN_INTERVAL}
                           BTM_DEFAULT_CONN_WINDOW=${WIIMOTE_BT_PAGE_SCAN_WINDOW}
                           BTM_DEFAULT_CONN_INTERVAL=${WIIMOTE_BT_PAGE_SCAN_INTERVAL}
                           # Role switch + hold + park, no sniff (0x0004): the DolphinBar
                           # rejects sniff, and Bluedroid otherwise retries it after every report.
                           BTA_DM_LINK_POLICY_SETTINGS=0x000B)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           WIIMOTE_BT_INQUIRY_SCAN_WINDOW=${WIIMOTE_BT_INQUIRY_SCAN_WINDOW}
                           WIIMOTE_BT_INQUIRY_SCAN_INTERVAL=${WIIMOTE_BT_INQUIRY_SCAN_INTERVAL}
                           WIIMOTE_BT_PAGE_SCAN_WINDOW=${WIIMOTE_BT_PAGE_SCAN_WINDOW}
                           WIIMOTE_BT_PAGE_SCAN_INTERVAL=${WIIMOTE_BT_PAGE_SCAN_INTERVAL})

# The registry BNO085 component uses legacy driver include paths; IDF 6 split these into driver components.
idf_component_get_property(BNO085_COMPONENT_LIB rinku404__bno085 COMPONENT_LIB)
idf_component_get_property(ESP_DRIVER_I2C_DIR esp_driver_i2c COMPONENT_DIR)
idf_component_get_property(ESP_DRIVER_GPIO_DIR esp_driver_gpio COMPONENT_DIR)
target_include_directories(${BNO085_COMPONENT_LIB} PRIVATE
                           "${ESP_DRIVER_I2C_DIR}/include"
                           "${ESP_DRIVER_GPIO_DIR}/include")
