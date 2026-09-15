#ifndef CODEOS_OPENWEB_CORE_H
#define CODEOS_OPENWEB_CORE_H

#include "ow_http.h"

/* C backend shared by the Qt frontend and legacy OpenWeb clients. */
void ow_core_init(void);
int  ow_core_is_initialized(void);
void ow_core_navigate(const char *text);
void ow_core_navigate_post(const char *action, const char *body, int body_len);
void ow_core_reload(void);
void ow_core_new_tab(void);
void ow_core_close_active_tab(void);
void ow_core_set_active_tab(int index);
void ow_core_render_active(void);
openweb_tab_t *ow_core_tabs(void);
int  ow_core_tab_count(void);
int  ow_core_active_tab(void);
int  ow_core_used_tab_count(void);
int  ow_core_load_progress(void);

#endif
