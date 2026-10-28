#set(COMPONENT_SRCS "main.c")
#set(COMPONENT_ADD_INCLUDEDIRS "")

#register_component()

idf_component_register(SRCS "wiimote_test.c" "wiimote_descriptor.c" "wiimote_identity.c"
                    PRIV_REQUIRES bt nvs_flash
                    INCLUDE_DIRS ".")
