#ifndef WEB_UI_H
#define WEB_UI_H

#include <stddef.h>

const char* web_ui_get_enhance_js_start(void);
size_t      web_ui_get_enhance_js_size(void);

const char* web_ui_get_bootstrap_css_start(void);
size_t      web_ui_get_bootstrap_css_size(void);

const char* web_ui_get_bootstrap_icons_css_start(void);
size_t      web_ui_get_bootstrap_icons_css_size(void);

const char* web_ui_get_font_woff2_start(void);
size_t      web_ui_get_font_woff2_size(void);

#endif
