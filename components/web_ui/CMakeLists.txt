idf_component_register(
    SRCS "web_ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server
    EMBED_FILES
        "frontend/js/enhance.js"
        "frontend/css/bootstrap.min.css"
        "frontend/css/bootstrap-icons.css"
        "frontend/css/fonts/bootstrap-icons.woff2"
)
