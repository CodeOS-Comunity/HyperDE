#include "openweb_core.h"
#include "ow_html.h"

#include <string.h>
#include "kprintf.h"

static int g_initialized;

void ow_core_init(void) {
    if (g_initialized) return;
    ow_tab_new("about:blank");
    g_initialized = 1;
}

int ow_core_is_initialized(void) { return g_initialized; }

void ow_core_navigate(const char *text) {
    if (!text || !text[0]) return;
    ow_core_init();
    kprintf("CORE nav text='%s'\n", text);
    if (!strchr(text, '.') && strncmp(text, "http", 4) != 0 &&
        strncmp(text, "about:", 6) != 0) {
        kprintf("CORE -> ow_search\n");
        ow_search(text);
        kprintf("CORE ow_search done\n");
    }
    else {
        kprintf("CORE -> ow_navigate\n");
        ow_navigate(text);
        kprintf("CORE ow_navigate done\n");
    }
}

void ow_core_navigate_post(const char *action, const char *body, int body_len) {
    if (!action || !action[0]) return;
    ow_core_init();
    kprintf("CORE POST action='%s' len=%d\n", action, body_len);
    ow_navigate_post(action, body, body_len);
}

void ow_core_reload(void) {
    int index;
    openweb_tab_t *tabs;
    ow_core_init();
    index = ow_get_tab_active();
    tabs = ow_get_tabs();
    if (tabs && index >= 0 && index < ow_get_tab_count() && tabs[index].url[0])
        ow_navigate_fresh(tabs[index].url);
}

void ow_core_new_tab(void) {
    ow_core_init();
    ow_tab_new("about:blank");
}

void ow_core_close_active_tab(void) {
    int index;
    if (!g_initialized || ow_get_tab_count() <= 1) return;
    index = ow_get_tab_active();
    if (index >= 0) ow_tab_close(index);
}

void ow_core_set_active_tab(int index) {
    if (g_initialized && index >= 0 && index < ow_get_tab_count())
        ow_set_tab_active(index);
}

void ow_core_render_active(void) {
    int index;
    openweb_tab_t *tabs;
    if (!g_initialized) return;
    index = ow_get_tab_active();
    tabs = ow_get_tabs();
    if (tabs && index >= 0 && index < ow_get_tab_count() &&
        tabs[index].content_len > 0 && tabs[index].content_len < OW_CONTENT_MAX)
        render_html(tabs[index].content, tabs[index].content_len);
}

openweb_tab_t *ow_core_tabs(void) { return g_initialized ? ow_get_tabs() : 0; }
int ow_core_tab_count(void) { return g_initialized ? ow_get_tab_count() : 0; }
int ow_core_active_tab(void) { return g_initialized ? ow_get_tab_active() : -1; }
int ow_core_used_tab_count(void) { return g_initialized ? ow_tab_used_count() : 0; }
int ow_core_load_progress(void) { return g_initialized ? ow_get_load_progress() : 0; }
