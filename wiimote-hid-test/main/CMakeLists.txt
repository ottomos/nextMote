#set(COMPONENT_SRCS "main.c")
#set(COMPONENT_ADD_INCLUDEDIRS "")

#register_component()

idf_component_register(SRCS "wiimote_test.c" "wiimote_descriptor.c" "wiimote_identity.c" "wiimote_web.c"
                    PRIV_REQUIRES bt esp_http_server esp_netif esp_wifi nvs_flash
                    INCLUDE_DIRS ".")
