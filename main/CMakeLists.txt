# Jak w przykładzie esp-matter (examples/light): bez jawnego REQUIRES komponent
# "main" widzi wszystkie komponenty (chip, openthread, esp_matter_console, ...).
# Wcześniejsza lista PRIV_REQUIRES gubiła część z nich.
idf_component_register(
    SRCS
        "app_main.cpp"
    PRIV_INCLUDE_DIRS
        "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
