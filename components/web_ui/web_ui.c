#include <stdio.h>
#include "web_ui.h"

extern const char _binary_enhance_js_start[];
extern const char _binary_enhance_js_end[];

extern const char _binary_bootstrap_min_css_start[];
extern const char _binary_bootstrap_min_css_end[];

extern const char _binary_bootstrap_icons_css_start[];
extern const char _binary_bootstrap_icons_css_end[];

extern const char _binary_bootstrap_icons_woff2_start[];
extern const char _binary_bootstrap_icons_woff2_end[];

const char* web_ui_get_enhance_js_start(void)
{
    return _binary_enhance_js_start;
}

size_t web_ui_get_enhance_js_size(void)
{
    return _binary_enhance_js_end - _binary_enhance_js_start;
}

const char* web_ui_get_bootstrap_css_start(void)
{
    return _binary_bootstrap_min_css_start;
}

size_t web_ui_get_bootstrap_css_size(void)
{
    return _binary_bootstrap_min_css_end - _binary_bootstrap_min_css_start;
}

const char* web_ui_get_bootstrap_icons_css_start(void)
{
    return _binary_bootstrap_icons_css_start;
}

size_t web_ui_get_bootstrap_icons_css_size(void)
{
    return _binary_bootstrap_icons_css_end - _binary_bootstrap_icons_css_start;
}

const char* web_ui_get_font_woff2_start(void)
{
    return _binary_bootstrap_icons_woff2_start;
}

size_t web_ui_get_font_woff2_size(void)
{
    return _binary_bootstrap_icons_woff2_end - _binary_bootstrap_icons_woff2_start;
}
