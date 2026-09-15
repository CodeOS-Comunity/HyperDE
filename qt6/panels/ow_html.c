#include "ow_html.h"
#include "string.h"
#include "kprintf.h"
#include "mm.h"
#include "net.h"

/* kernel HTTP(S) clients (declared in net.c) */
extern int https_get(const char *host, uint16_t port, const char *path, void *buf, uint16_t max_len);

/* ── Parallel image fetch infrastructure ── */
#include "spinlock.h"

enum {
    IMG_ST_EMPTY   = 0,
    IMG_ST_PENDING = 1,
    IMG_ST_FETCH   = 2,
    IMG_ST_READY   = 3,
    IMG_ST_FAIL    = 4,
};

static volatile int  s_img_state[OW_MAX_IMAGES];
static volatile int  s_img_raw_len[OW_MAX_IMAGES];
static unsigned char s_img_raw[OW_MAX_IMAGES][65535];
static char          s_img_absurl[OW_MAX_IMAGES][OW_URL_MAX];
static spinlock_t    s_img_lock = SPINLOCK_INIT;
static volatile int  s_img_workers_started;

int sched_create_thread(const char *name, void (*entry)(void));
void sched_sleep_ms(uint64_t ms);

static void ow_img_worker(void) {
    for (;;) {
        int idx = -1;
        char url_local[OW_URL_MAX];
        spin_lock(&s_img_lock);
        for (int i = 0; i < OW_MAX_IMAGES; i++) {
            if (s_img_state[i] == IMG_ST_PENDING) {
                s_img_state[i] = IMG_ST_FETCH;
                idx = i;
                int u = 0;
                while (u < OW_URL_MAX - 1 && s_img_absurl[i][u]) {
                    url_local[u] = s_img_absurl[i][u];
                    u++;
                }
                url_local[u] = 0;
                break;
            }
        }
        spin_unlock(&s_img_lock);

        if (idx < 0) { sched_sleep_ms(40); continue; }

        int n = ow_image_download(url_local, s_img_raw[idx], 65535);
        spin_lock(&s_img_lock);
        if (s_img_state[idx] == IMG_ST_FETCH) {
            s_img_raw_len[idx] = (n > 8) ? n : 0;
            s_img_state[idx]   = (n > 8) ? IMG_ST_READY : IMG_ST_FAIL;
        }
        spin_unlock(&s_img_lock);
    }
}

void ow_image_start_workers(void) {
    if (__sync_bool_compare_and_swap(&s_img_workers_started, 0, 1)) {
        sched_create_thread("ow-img-0", ow_img_worker);
        sched_create_thread("ow-img-1", ow_img_worker);
    }
}

void ow_image_enqueue(int idx, const char *abs_url) {
    if (idx < 0 || idx >= OW_MAX_IMAGES || !abs_url) return;
    spin_lock(&s_img_lock);
    if (s_img_state[idx] == IMG_ST_EMPTY || s_img_state[idx] == IMG_ST_FAIL) {
        s_img_state[idx] = IMG_ST_PENDING;
        int u = 0;
        while (u < OW_URL_MAX - 1 && abs_url[u]) { s_img_absurl[idx][u] = abs_url[u]; u++; }
        s_img_absurl[idx][u] = 0;
    }
    spin_unlock(&s_img_lock);
}

int ow_image_state(int idx) {
    if (idx < 0 || idx >= OW_MAX_IMAGES) return IMG_ST_EMPTY;
    return s_img_state[idx];
}

const unsigned char *ow_image_raw(int idx) {
    if (idx < 0 || idx >= OW_MAX_IMAGES) return 0;
    return s_img_raw[idx];
}

int ow_image_raw_len(int idx) {
    if (idx < 0 || idx >= OW_MAX_IMAGES) return 0;
    return s_img_raw_len[idx];
}

void ow_image_reset_all(void) {
    spin_lock(&s_img_lock);
    for (int i = 0; i < OW_MAX_IMAGES; i++) {
        s_img_state[i]   = IMG_ST_EMPTY;
        s_img_raw_len[i] = 0;
        s_img_absurl[i][0] = 0;
    }
    spin_unlock(&s_img_lock);
}

char ow_txt[OW_TXT_LINES][OW_TXT_COLS];
int  ow_txt_lines;
ow_link_t ow_links[OW_MAX_LINKS];
int  ow_link_cnt;
ow_image_t ow_images[OW_MAX_IMAGES];
int  ow_image_cnt;
int  ow_need_render;
ow_line_info_t ow_line_info[OW_TXT_LINES];
int  ow_line_img[OW_TXT_LINES];
char ow_page_title[OW_URL_MAX];

ow_form_t ow_forms[OW_MAX_FORMS];
int  ow_form_cnt;
ow_form_field_t ow_form_fields[OW_MAX_FIELDS];
int  ow_field_cnt;

/* Persistent per-control edit cache (keys name+type+form action). The renderer
 * is rebuilt on every paint, so user-typed values live here and are re-applied
 * to the matching new field on the next render. */
static char   s_fv_name[OW_MAX_FIELDS][OW_URL_MAX];
static uint8_t s_fv_type[OW_MAX_FIELDS];
static char   s_fv_action[OW_MAX_FIELDS][OW_URL_MAX];
static char   s_fv_value[OW_MAX_FIELDS][OW_URL_MAX];
static uint8_t s_fv_checked[OW_MAX_FIELDS];
static uint8_t s_fv_sel[OW_MAX_FIELDS];
static uint8_t s_fv_edited[OW_MAX_FIELDS];

/* Renderer state shared with the flush helper (per render_html() run). */
static int g_in_a;        /* inside an anchor */
static int g_cur_link;    /* link entry being filled (or -1) */
static int g_in_bold, g_in_italic, g_in_code;   /* open inline styles */
static int g_line_bold, g_line_italic, g_line_code; /* marks seen on pending line */
static int g_cell_type;   /* OW_LT_TH / OW_LT_TD while inside a table cell */

/* Form render state (per render_html() run). */
static int g_cur_form;        /* current <form> index, or -1 */
static int g_textarea_fi;     /* field index of open <textarea>, or -1 */
static int g_select_fi;       /* field index of open <select>, or -1 */
static int g_in_option;       /* inside an <option> capturing text */
static int g_opt_sel;         /* current option: is 'selected' */
static char g_opt_tmp[OW_SELECT_OPT_SZ];

static char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static void ow_strncpy_n(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int parse_int(const char *s) {
    int v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0');
    return v;
}

/* ─── attribute parser ─── */

typedef struct { char name[16]; char value[OW_URL_MAX]; } ow_attr_t;

/* Parse the attributes of a tag that starts at html[tg_start] ('<' is at
 * tg_start-1). Stops at the closing '>'. Returns the attribute count. */
static int parse_attrs(const char *html, int len, int tg_start, ow_attr_t *attrs, int max) {
    int p = tg_start, n = 0;
    /* skip the tag name itself */
    while (p < len && html[p] != '>' && html[p] != ' ' &&
           html[p] != '\t' && html[p] != '\n' && html[p] != '/') p++;
    while (p < len && html[p] != '>' && n < max) {
        while (p < len && html[p] != '>' &&
               (html[p] == ' ' || html[p] == '\t' || html[p] == '\n')) p++;
        if (p >= len || html[p] == '>' || html[p] == '/') break;
        int ni = 0;
        while (p < len && html[p] != '>' && html[p] != '=' && html[p] != ' ' &&
               html[p] != '\t' && html[p] != '\n' && ni < 15) {
            attrs[n].name[ni++] = lower_ascii(html[p++]);
        }
        attrs[n].name[ni] = 0;
        while (p < len && html[p] != '>' &&
               (html[p] == ' ' || html[p] == '\t' || html[p] == '\n')) p++;
        int vi = 0;
        if (p < len && html[p] == '=') {
            p++;
            while (p < len && html[p] != '>' &&
                   (html[p] == ' ' || html[p] == '\t' || html[p] == '\n')) p++;
            if (p < len && (html[p] == '"' || html[p] == '\'')) {
                char q = html[p++];
                while (p < len && html[p] != q && vi < OW_URL_MAX - 1)
                    attrs[n].value[vi++] = html[p++];
                if (p < len && html[p] == q) p++;
            } else {
                while (p < len && html[p] != '>' && html[p] != ' ' &&
                       html[p] != '\t' && html[p] != '\n' && vi < OW_URL_MAX - 1)
                    attrs[n].value[vi++] = html[p++];
            }
        }
        attrs[n].value[vi] = 0;
        n++;
    }
    return n;
}

/* ─── helpers ─── */

static void line_set_type(int ln, uint8_t t) {
    if (ln >= 0 && ln < OW_TXT_LINES) ow_line_info[ln].type = t;
}

/* Append n raw bytes to the pending line (bounds-checked). */
static void buf_put(char *buf, int *col, const char *s, int n) {
    for (int i = 0; i < n && *col < OW_TXT_COLS - 1; i++) buf[(*col)++] = s[i];
}

/* ─── form controls ─── */

static void flush_buf(char *buf, int *col);   /* defined below */

/* Map a low-case <input type> string to an OW_FT_* id. */
static int field_type_id(const char *t) {
    if (!t || !t[0]) return OW_FT_TEXT;
    if (strcmp(t, "password") == 0) return OW_FT_PASSWORD;
    if (strcmp(t, "submit") == 0) return OW_FT_SUBMIT;
    if (strcmp(t, "button") == 0) return OW_FT_BUTTON;
    if (strcmp(t, "checkbox") == 0) return OW_FT_CHECKBOX;
    if (strcmp(t, "radio") == 0) return OW_FT_RADIO;
    if (strcmp(t, "hidden") == 0 || strcmp(t, "file") == 0 ||
        strcmp(t, "image") == 0) return -1;    /* not rendered */
    /* email / number / search / tel / url / date ... render as text box */
    return OW_FT_TEXT;
}

static void opt_add(ow_form_field_t *f, const char *txt, int selected) {
    if (!f || f->opt_cnt >= OW_MAX_SELECT_OPTS) return;
    ow_strncpy_n(f->opts[f->opt_cnt], txt, OW_SELECT_OPT_SZ);
    if (selected) { f->opt_sel = f->opt_cnt; f->value[0] = 0; }
    if (f->opt_sel == f->opt_cnt && !f->value[0])
        ow_strncpy_n(f->value, f->opts[f->opt_cnt], OW_URL_MAX);
    f->opt_cnt++;
}

static void render_field_box(ow_form_field_t *f, char *buf, int *col) {
    int start = *col;
    if (start >= OW_TXT_COLS - 1) return;
    if (start > 0) { if (*col < OW_TXT_COLS - 1) buf[(*col)++] = ' '; }
    f->line = ow_txt_lines;
    f->col = *col;

    int vlen = (int)strlen(f->value);
    int w;
    const char *src = f->value;
    int center = 0;
    switch (f->type) {
        case OW_FT_CHECKBOX:
        case OW_FT_RADIO:
            buf_put(buf, col, f->checked ? "[x]" : "[ ]", 3);
            f->width = 3;
            return;
        case OW_FT_SUBMIT:
        case OW_FT_BUTTON:
            w = vlen + 4; src = f->value; center = 1; break;
        case OW_FT_SELECT:
            w = vlen + 4; src = f->value; break;
        default:
            w = vlen + 2;
            if (w < 8) w = 8;
            if (w > 24) w = 24;
            break;
    }
    if (w > OW_TXT_COLS - 1 - *col) w = OW_TXT_COLS - 1 - *col;
    if (w < 2) w = 2;

    buf_put(buf, col, "[", 1);
    int inner = w - 2;
    if (center) {
        int pad = (inner - vlen) / 2;
        if (pad < 0) pad = 0;
        for (int k = 0; k < pad && *col < OW_TXT_COLS - 1; k++) buf[(*col)++] = ' ';
    }
    for (int k = 0; src[k] && k < inner && *col < OW_TXT_COLS - 1; k++) {
        char c = src[k];
        if (f->type == OW_FT_PASSWORD && c != ' ') c = '*';
        buf[(*col)++] = c;
    }
    while (*col < start + 1 + inner && *col < OW_TXT_COLS - 1) buf[(*col)++] = ' ';
    if (f->type == OW_FT_SELECT && *col < OW_TXT_COLS - 1) buf[(*col)++] = 'v';
    if (f->type == OW_FT_SELECT && *col < OW_TXT_COLS - 1) buf[(*col)++] = ' ';
    while (*col < start + w - 1 && *col < OW_TXT_COLS - 1) buf[(*col)++] = ' ';
    if (*col < OW_TXT_COLS - 1) buf[(*col)++] = ']';
    f->width = *col - f->col;
}

/* Render a two-row textarea box. The first row shows the value; only the
 * first row participates in inline editing (field.line/col refer to row 1). */
static void render_textarea_box(ow_form_field_t *f, char *buf, int *col) {
    if (*col > 0) { if (*col < OW_TXT_COLS - 1) buf[(*col)++] = ' '; }
    f->line = ow_txt_lines;
    f->col = *col;
    int w = 36;
    int cn = f->col + 1;
    buf_put(buf, col, "(", 1);
    for (int k = 0; f->value[k] && k < w - 4 && *col < OW_TXT_COLS - 2; k++) {
        if (f->value[k] == '\n' || f->value[k] == '\r') break;
        buf[(*col)++] = f->value[k];
    }
    while (*col < f->col + w - 1 && *col < OW_TXT_COLS - 1) buf[(*col)++] = ' ';
    if (*col < OW_TXT_COLS - 1) buf[(*col)++] = ')';
    if (*col - f->col > w) w = *col - f->col;
    (void)cn;
    flush_buf(buf, col);
    if (*col < OW_TXT_COLS - 1) buf[(*col)++] = '(';
    for (int k = 0; k < w - 2 && *col < OW_TXT_COLS - 1; k++) buf[(*col)++] = ' ';
    if (*col < OW_TXT_COLS - 1) buf[(*col)++] = ')';
    f->width = w;
}

/* Find the form a field belongs to (or -1). */
static int field_form_idx(int fi) {
    for (int k = 0; k < ow_form_cnt; k++)
        if (fi >= ow_forms[k].field_start &&
            fi < ow_forms[k].field_start + ow_forms[k].field_count)
            return k;
    return -1;
}

/* The enclosing form's action (for the persistent cache key). */
static const char *field_form_action(int fi) {
    int k = field_form_idx(fi);
    return (k >= 0) ? ow_forms[k].action : "";
}

/* Apply a cached user edit to a freshly parsed field, if the field identity
 * (form action, name, type) matches the cache slot. */
static void fv_restore(ow_form_field_t *f, int fi) {
    if (fi < 0 || fi >= OW_MAX_FIELDS) return;
    if (!s_fv_edited[fi]) { s_fv_name[fi][0] = 0; return; }
    if (strcmp(s_fv_name[fi], f->name) != 0 || s_fv_type[fi] != f->type ||
        strcmp(s_fv_action[fi], field_form_action(fi)) != 0) {
        s_fv_edited[fi] = 0;
        s_fv_name[fi][0] = 0;
        return;
    }
    if (f->type == OW_FT_CHECKBOX || f->type == OW_FT_RADIO) {
        f->checked = s_fv_checked[fi] ? 1 : 0;
        if (f->type == OW_FT_RADIO && f->checked)
            ow_strncpy_n(f->value, s_fv_value[fi], OW_URL_MAX);
    } else if (f->type == OW_FT_SELECT) {
        f->value[0] = 0;
        if (s_fv_sel[fi] < f->opt_cnt) {
            f->opt_sel = s_fv_sel[fi];
            ow_strncpy_n(f->value, f->opts[f->opt_sel], OW_URL_MAX);
        }
    } else {
        ow_strncpy_n(f->value, s_fv_value[fi], OW_URL_MAX);
    }
}

/* Persist the field's current identity + values into the cache slot. */
static void fv_store(const ow_form_field_t *f, int fi) {
    if (fi < 0 || fi >= OW_MAX_FIELDS) return;
    ow_strncpy_n(s_fv_name[fi], f->name, OW_URL_MAX);
    s_fv_type[fi] = f->type;
    ow_strncpy_n(s_fv_action[fi], field_form_action(fi), OW_URL_MAX);
    if (f->type != OW_FT_CHECKBOX)
        ow_strncpy_n(s_fv_value[fi], f->value, OW_URL_MAX);
    if (s_fv_edited[fi]) {
        if (f->type == OW_FT_CHECKBOX) s_fv_checked[fi] = f->checked ? 1 : 0;
    } else {
        s_fv_checked[fi] = f->checked ? 1 : 0;
    }
    s_fv_sel[fi] = f->opt_sel;
}

/* ─── exported form API ─── */

void ow_field_set_value(int fi, const char *v) {
    ow_form_field_t *f;
    if (fi < 0 || fi >= ow_field_cnt) return;
    f = &ow_form_fields[fi];
    if (!v) v = "";
    if (f->type == OW_FT_SELECT) {
        uint8_t k;
        for (k = 0; k < f->opt_cnt; k++) {
            if (strcmp(f->opts[k], v) == 0) {
                f->opt_sel = k;
                ow_strncpy_n(f->value, f->opts[k], OW_URL_MAX);
                s_fv_edited[fi] = 1;
                s_fv_sel[fi] = k;
                return;
            }
        }
        f->opt_sel = (f->opt_cnt) ? (uint8_t)(f->opt_cnt - 1) : 0;
        return;
    }
    ow_strncpy_n(f->value, v, OW_URL_MAX);
    s_fv_edited[fi] = 1;
    ow_strncpy_n(s_fv_value[fi], v, OW_URL_MAX);
}

void ow_field_toggle(int fi) {
    ow_form_field_t *f;
    if (fi < 0 || fi >= ow_field_cnt) return;
    f = &ow_form_fields[fi];
    if (f->type == OW_FT_CHECKBOX) {
        f->checked = f->checked ? 0 : 1;
        s_fv_edited[fi] = 1;
        s_fv_checked[fi] = f->checked ? 1 : 0;
    } else if (f->type == OW_FT_RADIO) {
        int form = f->form;
        int k;
        for (k = 0; k < ow_field_cnt; k++) {
            ow_form_field_t *r = &ow_form_fields[k];
            if (r->type == OW_FT_RADIO && r->form == form &&
                strcmp(r->name, f->name) == 0) {
                if (k == fi) {
                    r->checked = 1;
                    s_fv_checked[k] = 1;
                    s_fv_edited[k] = 1;
                    ow_strncpy_n(s_fv_value[k], r->value, OW_URL_MAX);
                    s_fv_edited[fi] = 1;
                } else {
                    r->checked = 0;
                    s_fv_checked[k] = 0;
                }
            }
        }
    }
}

int ow_field_at(int line, int col) {
    int k;
    if (line < 0) return -1;
    for (k = 0; k < ow_field_cnt; k++) {
        ow_form_field_t *f = &ow_form_fields[k];
        if (f->line == line && col >= f->col && col < f->col + f->width)
            return k;
    }
    return -1;
}

/* URL-encode one name/value pair into out; returns bytes written. */
static int qpart(char *out, int out_max, const char *name, const char *val) {
    static const char hex[] = "0123456789ABCDEF";
    static const char unres[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
    int i = 0, n = 0;
    const char *s;
    for (s = name; *s; s++) {
        if (n + 3 >= out_max) return n;
        if (strchr(unres, *s)) out[n++] = *s;
        else { out[n++] = '%'; out[n++] = hex[((unsigned char)*s) >> 4]; out[n++] = hex[((unsigned char)*s) & 15]; }
    }
    if (n + 1 >= out_max) return n;
    out[n++] = '=';
    for (s = val; *s; s++) {
        if (n + 3 >= out_max) return n;
        if (*s == ' ') out[n++] = '+';
        else if (strchr(unres, *s)) out[n++] = *s;
        else { out[n++] = '%'; out[n++] = hex[((unsigned char)*s) >> 4]; out[n++] = hex[((unsigned char)*s) & 15]; }
    }
    (void)i;
    return n;
}

int ow_form_build_query(const ow_form_t *f, char *out, int out_max) {
    int n = 0, k;
    if (!f || !out || out_max <= 0) return 0;
    out[0] = 0;
    for (k = f->field_start; k < f->field_start + f->field_count && k < ow_field_cnt; k++) {
        ow_form_field_t *fd = &ow_form_fields[k];
        if (fd->type == OW_FT_SUBMIT || fd->type == OW_FT_BUTTON) continue;
        if ((fd->type == OW_FT_CHECKBOX || fd->type == OW_FT_RADIO) && !fd->checked) continue;
        if (!fd->name[0]) continue;
        if (n) { if (n + 1 >= out_max) break; out[n++] = '&'; }
        {
            const char *qv = fd->value;
            if ((fd->type == OW_FT_CHECKBOX || fd->type == OW_FT_RADIO) && !qv[0]) qv = "on";
            n += qpart(out + n, out_max - n, fd->name, qv);
        }
    }
    out[n] = 0;
    return n;
}

/* Encode a Unicode code point as UTF-8. Returns byte count. */
static int utf8_encode(unsigned int cp, unsigned char *out) {
    if (cp < 0x80)     { out[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800)    { out[0] = (unsigned char)(0xC0 | (cp >> 6));
                         out[1] = (unsigned char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000)  { out[0] = (unsigned char)(0xE0 | (cp >> 12));
                         out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                         out[2] = (unsigned char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (unsigned char)(0xF0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

static const struct { const char *name; const unsigned char u[4]; unsigned char ul; } ENT[] = {
    {"&amp;",   {'&'}, 1},
    {"&lt;",    {'<'}, 1},
    {"&gt;",    {'>'}, 1},
    {"&quot;",  {'"'}, 1},
    {"&apos;",  {'\''}, 1},
    {"&nbsp;",  {' '}, 1},
    {"&copy;",  {0xC2,0xA9}, 2}, {"&reg;", {0xC2,0xAE}, 2},
    {"&trade;", {0xE2,0x84,0xA2}, 3}, {"&bull;", {0xE2,0x80,0xA2}, 3},
    {"&middot;",{0xC2,0xB7}, 2}, {"&ndash;", {0xE2,0x80,0x93}, 3},
    {"&mdash;", {0xE2,0x80,0x94}, 3}, {"&hellip;", {0xE2,0x80,0xA6}, 3},
    {"&larr;",  {0xE2,0x86,0x90}, 3}, {"&rarr;", {0xE2,0x86,0x92}, 3},
    {"&uarr;",  {0xE2,0x86,0x91}, 3}, {"&darr;", {0xE2,0x86,0x93}, 3},
    {"&times;", {0xC3,0x97}, 2}, {"&divide;", {0xC3,0xB7}, 2},
    {"&euro;",  {0xE2,0x82,0xAC}, 3}, {"&pound;", {0xC2,0xA3}, 2},
    {"&yen;",   {0xC2,0xA5}, 2}, {"&cent;", {0xC2,0xA2}, 2},
    {"&lsquo;", {0xE2,0x80,0x98}, 3}, {"&rsquo;", {0xE2,0x80,0x99}, 3},
    {"&ldquo;", {0xE2,0x80,0x9C}, 3}, {"&rdquo;", {0xE2,0x80,0x9D}, 3},
    {"&laquo;", {0xC2,0xAB}, 2}, {"&raquo;", {0xC2,0xBB}, 2},
    {"&lsaquo;",{0xE2,0x80,0xB9}, 3}, {"&rsaquo;",{0xE2,0x80,0xBA}, 3},
    {"&sbquo;", {0xE2,0x80,0x9A}, 3}, {"&bdquo;", {0xE2,0x80,0x9E}, 3},
    {"&sect;",  {0xC2,0xA7}, 2}, {"&para;", {0xC2,0xB6}, 2},
    {"&deg;",   {0xC2,0xB0}, 2}, {"&plusmn;",{0xC2,0xB1}, 2},
    {"&micro;", {0xC2,0xB5}, 2}, {"&sup2;", {0xC2,0xB2}, 2},
    {"&sup3;",  {0xC2,0xB3}, 2}, {"&frac12;",{0xC2,0xBD}, 2},
    {"&frac14;",{0xC2,0xBC}, 2}, {"&frac34;",{0xC2,0xBE}, 2},
    {"&acute;", {0xC2,0xB4}, 2}, {"&uml;", {0xC2,0xA8}, 2},
    {"&cedil;", {0xC2,0xB8}, 2}, {"&ordm;", {0xC2,0xBA}, 2},
    {"&ordf;",  {0xC2,0xAA}, 2}, {"&not;", {0xC2,0xAC}, 2},
    {"&brvbar;",{0xC2,0xA6}, 2}, {"&iquest;",{0xC2,0xBF}, 2},
    {"&iexcl;", {0xC2,0xA1}, 2},
    {"&spades;",{0xE2,0x99,0xA0}, 3}, {"&clubs;", {0xE2,0x99,0xA3}, 3},
    {"&hearts;",{0xE2,0x99,0xA5}, 3}, {"&diams;", {0xE2,0x99,0xA6}, 3},
    {"&infin;", {0xE2,0x88,0x9E}, 3}, {"&ne;", {0xE2,0x89,0xA0}, 3},
    {"&le;",    {0xE2,0x89,0xA4}, 3}, {"&ge;", {0xE2,0x89,0xA5}, 3},
    /* Latin-1 accented */
    {"&agrave;",{0xC3,0xA0}, 2}, {"&aacute;",{0xC3,0xA1}, 2},
    {"&acirc;", {0xC3,0xA2}, 2}, {"&atilde;",{0xC3,0xA3}, 2},
    {"&auml;",  {0xC3,0xA4}, 2}, {"&aring;", {0xC3,0xA5}, 2},
    {"&aelig;", {0xC3,0xA6}, 2}, {"&ccedil;",{0xC3,0xA7}, 2},
    {"&egrave;",{0xC3,0xA8}, 2}, {"&eacute;",{0xC3,0xA9}, 2},
    {"&ecirc;", {0xC3,0xAA}, 2}, {"&euml;",  {0xC3,0xAB}, 2},
    {"&igrave;",{0xC3,0xAC}, 2}, {"&iacute;",{0xC3,0xAD}, 2},
    {"&icirc;", {0xC3,0xAE}, 2}, {"&iuml;",  {0xC3,0xAF}, 2},
    {"&ntilde;",{0xC3,0xB1}, 2}, {"&ograve;",{0xC3,0xB2}, 2},
    {"&oacute;",{0xC3,0xB3}, 2}, {"&ocirc;", {0xC3,0xB4}, 2},
    {"&otilde;",{0xC3,0xB5}, 2}, {"&ouml;",  {0xC3,0xB6}, 2},
    {"&oslash;",{0xC3,0xB8}, 2}, {"&ugrave;",{0xC3,0xB9}, 2},
    {"&uacute;",{0xC3,0xBA}, 2}, {"&ucirc;", {0xC3,0xBB}, 2},
    {"&uuml;",  {0xC3,0xBC}, 2}, {"&yacute;",{0xC3,0xBD}, 2},
    {"&szlig;", {0xC3,0x9F}, 2},
};

/* Try to decode the entity starting at html[i] ('&'). On success returns the
 * number of consumed bytes (34-style numeric or named) and fills out[0..),*ol. */
static int try_entity(const char *html, int len, int i, unsigned char *out, int *ol) {
    if (i + 2 < len && html[i + 1] == '#') {
        int base = 10, j = i + 2;
        if (html[j] == 'x' || html[j] == 'X') { base = 16; j++; }
        if (j >= len || j - i > 12) return 0;
        unsigned int cp = 0;
        int digits = 0;
        while (j < len && digits < 6) {
            int v = -1;
            char c = html[j];
            if (base == 16) {
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            } else if (c >= '0' && c <= '9') {
                v = c - '0';
            }
            if (v < 0) break;
            cp = cp * base + (unsigned int)v;
            digits++;
            j++;
        }
        if (digits == 0 || j >= len || html[j] != ';') return 0;
        if (cp == 0 || cp > 0x10FFFF) return 0;
        *ol = utf8_encode(cp, out);
        return j - i + 1;
    }
    for (size_t e = 0; e < sizeof(ENT) / sizeof(ENT[0]); e++) {
        int n = (int)strlen(ENT[e].name);
        if (i + n <= len && strncmp(html + i, ENT[e].name, n) == 0) {
            memcpy(out, ENT[e].u, ENT[e].ul);
            *ol = ENT[e].ul;
            return n;
        }
    }
    return 0;
}

static void flush_buf(char *buf, int *col) {
    if (*col == 0) return;
    if (g_in_a && g_cur_link >= 0 && g_cur_link < OW_MAX_LINKS)
        ow_links[g_cur_link].ec = *col;
    buf[*col] = 0;
    int ln = ow_txt_lines;
    if (ln >= OW_TXT_LINES) {
        g_line_bold = g_line_italic = g_line_code = 0;
        *col = 0;
        return;
    }
    int j;
    for (j = 0; j < OW_TXT_COLS - 1 && buf[j]; j++)
        ow_txt[ln][j] = buf[j];
    ow_txt[ln][j] = 0;
    if (ow_line_info[ln].type == OW_LT_NORMAL) {
        if (g_cell_type) line_set_type(ln, (uint8_t)g_cell_type);
        else if (g_line_code) line_set_type(ln, OW_LT_CODE);
        else if (g_line_bold) line_set_type(ln, OW_LT_BOLD);
        else if (g_line_italic) line_set_type(ln, OW_LT_ITALIC);
    }
    /* continue a spanning anchor onto the new line */
    if (g_in_a && g_cur_link >= 0 && g_cur_link < OW_MAX_LINKS &&
        ow_link_cnt < OW_MAX_LINKS) {
        memcpy(&ow_links[ow_link_cnt], &ow_links[g_cur_link], sizeof(ow_link_t));
        ow_links[ow_link_cnt].line = ln + 1;
        ow_links[ow_link_cnt].sc = 0;
        ow_links[ow_link_cnt].ec = 0;
        g_cur_link = ow_link_cnt;
        ow_link_cnt++;
    }
    ow_txt_lines = ln + 1;
    g_line_bold = g_line_italic = g_line_code = 0;
    *col = 0;
}

static void add_blank_line(void) {
    if (ow_txt_lines > 0 && ow_txt[ow_txt_lines - 1][0] != 0) {
        if (ow_txt_lines < OW_TXT_LINES) {
            ow_txt[ow_txt_lines][0] = 0;
            line_set_type(ow_txt_lines, OW_LT_EMPTY);
            ow_txt_lines++;
        }
    }
}

static int tag_match(const char *html, int i, int len, const char *tag) {
    /* check if at position i we have <tag or </tag */
    if (!html || i < 0 || i >= len || html[i] != '<') return 0;
    int is_close = (i + 1 < len && html[i+1] == '/');
    const char *t = tag;
    int pos = is_close ? i + 2 : i + 1;
    while (*t && pos < len && lower_ascii(html[pos]) == lower_ascii(*t)) { t++; pos++; }
    if (*t) return 0;
    /* tag must be followed by >, space, /, or end */
    if (pos >= len) return 0;
    char n = html[pos];
    return (n == '>' || n == ' ' || n == '\t' || n == '/') ? (is_close ? 2 : 1) : 0;
}

static int tag_match_exact(const char *html, int i, int len, const char *tag) {
    if (!html || i < 0 || i >= len || html[i] != '<') return 0;
    int is_close = (i + 1 < len && html[i+1] == '/');
    const char *t = tag;
    int pos = is_close ? i + 2 : i + 1;
    while (*t && pos < len && lower_ascii(html[pos]) == lower_ascii(*t)) { t++; pos++; }
    if (*t) return 0;
    if (pos < len && html[pos] == '>') return is_close ? 2 : 1;
    return 0;
}

/* skip to just past the '>' closing the tag at position i */
static int skip_tag_end(const char *html, int len, int i) {
    while (i < len && html[i] != '>') i++;
    if (i < len) i++;
    return i;
}

/* generic block tags: treated as paragraph boundaries */
static const char *BLOCK_TAGS[] = {
    "div", "section", "article", "aside", "header", "footer", "nav",
    "main", "center", "fieldset", "figure", "hgroup",
};

/* inline no-op tags (formatting our text grid cannot express) */
static const char *INLINE_TAGS[] = {
    "u", "s", "strike", "mark", "small", "big", "sub", "sup", "abbr",
    "cite", "var", "dfn", "time", "acronym", "tt", "kbd", "samp", "span",
};

/* container elements whose inner text is not site content */
static const char *SKIP_TAGS[] = {
    "script", "style", "noscript", "svg", "audio", "video", "iframe",
    "canvas", "map", "picture", "template", "object", "select",
    "optgroup", "datalist", "rp", "rt", "math",
};

/* ─── main renderer ─── */

void render_html(const char *html, int len) {
    int ti;
    for (ti = 0; ti < OW_TXT_LINES; ti++) {
        ow_txt[ti][0] = 0;
        ow_line_info[ti].type = OW_LT_NORMAL;
        ow_line_img[ti] = -1;
    }
    ow_txt_lines = 0;
    ow_link_cnt = 0;
    ow_image_cnt = 0;
    ow_image_reset_all();
    ow_page_title[0] = 0;

    g_in_a = 0; g_cur_link = -1;
    g_in_bold = g_in_italic = g_in_code = 0;
    g_line_bold = g_line_italic = g_line_code = 0;
    g_cell_type = 0;
    g_cur_form = -1;
    g_textarea_fi = -1;
    g_select_fi = -1;
    g_in_option = 0;
    g_opt_tmp[0] = 0;
    ow_form_cnt = 0;
    ow_field_cnt = 0;

    char buf[OW_TXT_COLS];
    int col = 0;
    int h_level = 0;
    int list_type = 0, ol_count = 0;
    int in_pre = 0;
    int in_title = 0;
    int in_row = 0, cell_count = 0;

    int i = 0;
    while (i < len && ow_txt_lines < OW_TXT_LINES) {
        if (html[i] == '<') {
            /* ── comments / doctype / CDATA ── */
            if (i + 3 < len && strncmp(html + i, "<!--", 4) == 0) {
                int j = i + 4;
                while (j + 2 < len && !(html[j] == '-' && html[j+1] == '-' && html[j+2] == '>')) j++;
                i = (j + 2 < len) ? j + 3 : len;
                continue;
            }
            if (i + 1 < len && (html[i+1] == '!' || html[i+1] == '?')) {
                int j = i + 2;
                if (html[i+1] == '!' && i + 8 < len && strncmp(html + i, "<![CDATA[", 9) == 0) {
                    while (j + 2 < len && !(html[j] == ']' && html[j+1] == ']' && html[j+2] == '>')) j++;
                    i = (j + 2 < len) ? j + 3 : len;
                } else {
                    while (j < len && html[j] != '>') j++;
                    i = (j < len) ? j + 1 : len;
                }
                continue;
            }

            /* ── <title> capture (no output) ── */
            if (tag_match_exact(html, i, len, "title") == 1) {
                in_title = 1;
                i = skip_tag_end(html, len, i);
                continue;
            }
            if (tag_match_exact(html, i, len, "title") == 2) {
                in_title = 0;
                i = skip_tag_end(html, len, i);
                continue;
            }
            if (tag_match_exact(html, i, len, "head") == 1 ||
                tag_match_exact(html, i, len, "head") == 2 ||
                tag_match(html, i, len, "base") == 1 ||
                tag_match(html, i, len, "meta") == 1 ||
                tag_match(html, i, len, "link") == 1 ||
                tag_match(html, i, len, "source") == 1 ||
                tag_match(html, i, len, "wbr") == 1) {
                goto skip_tag;
            }
            /* ── Heading tags ── */
            {
                int hm = 0;
                if (tag_match(html, i, len, "h1") == 1) hm = 1;
                else if (tag_match(html, i, len, "h2") == 1) hm = 2;
                else if (tag_match(html, i, len, "h3") == 1) hm = 3;
                else if (tag_match(html, i, len, "h4") == 1) hm = 4;
                else if (tag_match(html, i, len, "h5") == 1) hm = 5;
                else if (tag_match(html, i, len, "h6") == 1) hm = 6;
                if (hm) {
                    flush_buf(buf, &col); add_blank_line();
                    h_level = hm;
                    goto skip_tag;
                }
            }
            if (tag_match(html, i, len, "h1") == 2 || tag_match(html, i, len, "h2") == 2 ||
                tag_match(html, i, len, "h3") == 2 || tag_match(html, i, len, "h4") == 2 ||
                tag_match(html, i, len, "h5") == 2 || tag_match(html, i, len, "h6") == 2) {
                flush_buf(buf, &col);
                if (ow_txt_lines > 0) line_set_type(ow_txt_lines - 1, (uint8_t)(OW_LT_H1 - 1 + h_level));
                add_blank_line();
                h_level = 0;
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── Paragraph ── */
            if (tag_match(html, i, len, "p") == 1) {
                flush_buf(buf, &col); add_blank_line();
                goto skip_tag;
            }
            if (tag_match(html, i, len, "p") == 2) {
                flush_buf(buf, &col); add_blank_line();
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── Line break ── */
            if (tag_match(html, i, len, "br")) {
                flush_buf(buf, &col);
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── Lists ── */
            if (tag_match(html, i, len, "ul") == 1) {
                list_type = 1; goto skip_tag;
            }
            if (tag_match(html, i, len, "ol") == 1) {
                list_type = 2; ol_count = 0; goto skip_tag;
            }
            if (tag_match(html, i, len, "ul") == 2 || tag_match(html, i, len, "ol") == 2) {
                flush_buf(buf, &col); add_blank_line();
                list_type = 0;
                i = skip_tag_end(html, len, i);
                continue;
            }
            if (tag_match(html, i, len, "li") == 1) {
                flush_buf(buf, &col);
                if (list_type == 2) ol_count++;
                if (list_type == 1) {
                    buf_put(buf, &col, "\xE2\x80\xA2 ", 4);
                } else if (list_type == 2) {
                    char num[8]; int ni = 0, n = ol_count;
                    while (n) { num[ni++] = '0' + n % 10; n /= 10; }
                    while (ni > 0 && col < OW_TXT_COLS - 1) buf[col++] = num[--ni];
                    buf_put(buf, &col, ". ", 2);
                } else {
                    buf_put(buf, &col, "* ", 2);
                }
                goto skip_tag;
            }
            if (tag_match(html, i, len, "li") == 2) {
                flush_buf(buf, &col);
                if (ow_txt_lines > 0) line_set_type(ow_txt_lines - 1, OW_LT_LI);
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── Horizontal rule ── */
            if (tag_match(html, i, len, "hr")) {
                flush_buf(buf, &col);
                int ln = ow_txt_lines;
                if (ln < OW_TXT_LINES) {
                    int k;
                    for (k = 0; k < OW_TXT_COLS - 1; k++) ow_txt[ln][k] = '=';
                    ow_txt[ln][k] = 0;
                    line_set_type(ln, OW_LT_HR);
                    ow_txt_lines = ln + 1;
                }
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── Blockquote ── */
            if (tag_match_exact(html, i, len, "blockquote") == 1) {
                flush_buf(buf, &col); add_blank_line();
                int ln = ow_txt_lines;
                if (ln < OW_TXT_LINES) {
                    ow_txt[ln][0] = '>';
                    ow_txt[ln][1] = ' ';
                    ow_txt[ln][2] = 0;
                    for (int li = 2; li < OW_TXT_COLS - 1; li++) ow_txt[ln][li] = ' ';
                    ow_txt[ln][OW_TXT_COLS-1] = 0;
                    line_set_type(ln, OW_LT_BQ);
                    ow_txt_lines = ln + 1;
                }
                goto skip_tag;
            }
            if (tag_match_exact(html, i, len, "blockquote") == 2) {
                flush_buf(buf, &col); add_blank_line();
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── Code (inline and <pre> blocks) ── */
            if (tag_match(html, i, len, "code") == 1 && !in_pre) {
                g_in_code = 1; goto skip_tag;
            }
            if (tag_match_exact(html, i, len, "code") == 2 && !in_pre) {
                g_in_code = 0; goto skip_tag;
            }

            /* ── address (italic) ── */
            if (tag_match(html, i, len, "address") == 1) {
                flush_buf(buf, &col); add_blank_line();
                g_in_italic = 1; goto skip_tag;
            }
            if (tag_match(html, i, len, "address") == 2) {
                g_in_italic = 0; flush_buf(buf, &col); add_blank_line();
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── figcaption / legend / caption (italic inline labels) ── */
            if (tag_match(html, i, len, "figcaption") == 1 || tag_match(html, i, len, "legend") == 1 ||
                tag_match(html, i, len, "caption") == 1) {
                flush_buf(buf, &col);
                g_in_italic = 1; goto skip_tag;
            }
            if (tag_match(html, i, len, "figcaption") == 2 || tag_match(html, i, len, "legend") == 2 ||
                tag_match(html, i, len, "caption") == 2) {
                g_in_italic = 0; flush_buf(buf, &col);
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── Definition lists ── */
            if (tag_match(html, i, len, "dl") == 1) {
                flush_buf(buf, &col); add_blank_line(); goto skip_tag;
            }
            if (tag_match(html, i, len, "dl") == 2) {
                flush_buf(buf, &col); add_blank_line();
                i = skip_tag_end(html, len, i);
                continue;
            }
            if (tag_match(html, i, len, "dt") == 1) {
                flush_buf(buf, &col);
                g_in_bold = 1; goto skip_tag;
            }
            if (tag_match(html, i, len, "dt") == 2) {
                g_in_bold = 0; flush_buf(buf, &col);
                i = skip_tag_end(html, len, i);
                continue;
            }
            if (tag_match(html, i, len, "dd") == 1) {
                flush_buf(buf, &col);
                buf_put(buf, &col, "    ", 4);
                goto skip_tag;
            }
            if (tag_match(html, i, len, "dd") == 2) {
                flush_buf(buf, &col);
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── details / summary ── */
            if (tag_match(html, i, len, "details") == 1) {
                flush_buf(buf, &col); add_blank_line(); goto skip_tag;
            }
            if (tag_match(html, i, len, "details") == 2) {
                flush_buf(buf, &col); add_blank_line();
                i = skip_tag_end(html, len, i);
                continue;
            }
            if (tag_match(html, i, len, "summary") == 1) {
                flush_buf(buf, &col);
                buf_put(buf, &col, "\xE2\x96\xB8 ", 4);
                goto skip_tag;
            }
            if (tag_match(html, i, len, "summary") == 2) {
                flush_buf(buf, &col);
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── generic block tags ── */
            {
                int isblock = 0;
                for (size_t bi = 0; bi < sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]); bi++) {
                    if (tag_match(html, i, len, BLOCK_TAGS[bi])) { isblock = 1; break; }
                }
                if (isblock) {
                    flush_buf(buf, &col); add_blank_line();
                    goto skip_tag;
                }
            }

            /* ── Text formatting ── */
            if (tag_match(html, i, len, "b") == 1 || tag_match(html, i, len, "strong") == 1) {
                g_in_bold = 1; goto skip_tag;
            }
            if (tag_match(html, i, len, "b") == 2 || tag_match(html, i, len, "strong") == 2) {
                g_in_bold = 0; goto skip_tag;
            }
            if (tag_match(html, i, len, "i") == 1 || tag_match(html, i, len, "em") == 1) {
                g_in_italic = 1; goto skip_tag;
            }
            if (tag_match(html, i, len, "i") == 2 || tag_match(html, i, len, "em") == 2) {
                g_in_italic = 0; goto skip_tag;
            }
            /* <q> inline quotation marks */
            if (tag_match(html, i, len, "q") == 1) {
                buf_put(buf, &col, "\xE2\x80\x9C", 3);
                goto skip_tag;
            }
            if (tag_match_exact(html, i, len, "q") == 2) {
                buf_put(buf, &col, "\xE2\x80\x9D", 3);
                goto skip_tag;
            }
            {
                int isinline = 0;
                for (size_t ii = 0; ii < sizeof(INLINE_TAGS) / sizeof(INLINE_TAGS[0]); ii++) {
                    if (tag_match(html, i, len, INLINE_TAGS[ii])) { isinline = 1; break; }
                }
                if (isinline) goto skip_tag;
            }

            /* ── Table tags ── */
            if (tag_match(html, i, len, "table") == 1) {
                flush_buf(buf, &col); add_blank_line();
                goto skip_tag;
            }
            if (tag_match_exact(html, i, len, "table") == 2) {
                flush_buf(buf, &col); add_blank_line();
                in_row = 0; cell_count = 0; g_cell_type = 0;
                i = skip_tag_end(html, len, i);
                continue;
            }
            if (tag_match(html, i, len, "tr") == 1) {
                flush_buf(buf, &col);
                in_row = 1; cell_count = 0; g_cell_type = 0;
                goto skip_tag;
            }
            if (tag_match_exact(html, i, len, "tr") == 2) {
                flush_buf(buf, &col);
                in_row = 0; g_cell_type = 0;
                i = skip_tag_end(html, len, i);
                continue;
            }
            if (tag_match(html, i, len, "th") == 1 || tag_match(html, i, len, "td") == 1) {
                if (in_row) {
                    if (cell_count > 0) buf_put(buf, &col, " | ", 3);
                    cell_count++;
                }
                g_cell_type = (tag_match(html, i, len, "th") == 1) ? OW_LT_TH : OW_LT_TD;
                goto skip_tag;
            }
            if (tag_match(html, i, len, "th") == 2 || tag_match(html, i, len, "td") == 2) {
                goto skip_tag;
            }

            /* ── Image tags ── */
            if (tag_match(html, i, len, "img") == 1) {
                if (ow_image_cnt < OW_MAX_IMAGES) {
                    ow_image_t *img = &ow_images[ow_image_cnt];
                    memset(img, 0, sizeof(ow_image_t));
                    char alt[OW_TXT_COLS];
                    alt[0] = 0;
                    ow_attr_t at[16];
                    int na = parse_attrs(html, len, i + 1, at, 16);
                    for (int k = 0; k < na; k++) {
                        if (strcmp(at[k].name, "src") == 0)
                            ow_strncpy_n(img->url, at[k].value, OW_URL_MAX);
                        else if (strcmp(at[k].name, "width") == 0)
                            img->w = parse_int(at[k].value);
                        else if (strcmp(at[k].name, "height") == 0)
                            img->h = parse_int(at[k].value);
                        else if (strcmp(at[k].name, "alt") == 0) {
                            if (at[k].value[0]) {
                                int w = (int)strlen(at[k].value);
                                if (w > 36) w = 36;
                                memcpy(alt, at[k].value, (size_t)w);
                                alt[w] = 0;
                            }
                        }
                    }
                    flush_buf(buf, &col);
                    int ln = ow_txt_lines;
                    if (ln < OW_TXT_LINES) {
                        line_set_type(ln, OW_LT_IMAGE);
                        int ai = 0;
                        if (alt[0]) {
                            ow_txt[ln][ai++] = '['; ow_txt[ln][ai++] = 'I';
                            ow_txt[ln][ai++] = ']'; ow_txt[ln][ai++] = ' ';
                            for (int bi = 0; alt[bi] && ai < OW_TXT_COLS - 1; bi++)
                                ow_txt[ln][ai++] = alt[bi];
                        } else {
                            ow_txt[ln][ai++] = '['; ow_txt[ln][ai++] = 'I';
                            ow_txt[ln][ai++] = 'M'; ow_txt[ln][ai++] = 'G';
                            ow_txt[ln][ai++] = ']';
                        }
                        ow_txt[ln][ai] = 0;
                        ow_line_img[ln] = ow_image_cnt;
                        ow_txt_lines = ln + 1;
                    }
                    ow_image_cnt++;
                }
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── Anchor tags ── */
            if (!g_in_a && tag_match(html, i, len, "a") == 1) {
                if (ow_link_cnt < OW_MAX_LINKS) {
                    ow_attr_t at[8];
                    int na = parse_attrs(html, len, i + 1, at, 8);
                    const char *href = 0;
                    for (int k = 0; k < na; k++)
                        if (strcmp(at[k].name, "href") == 0) { href = at[k].value; break; }
                    if (href && href[0]) {
                        g_cur_link = ow_link_cnt;
                        ow_strncpy_n(ow_links[g_cur_link].url, href, OW_URL_MAX);
                        ow_links[g_cur_link].line = -1;
                        ow_links[g_cur_link].sc = 0;
                        ow_links[g_cur_link].ec = 0;
                        ow_link_cnt++;
                        g_in_a = 1;
                    }
                }
                i = skip_tag_end(html, len, i);
                continue;
            }
            if (g_in_a && tag_match(html, i, len, "a") == 2) {
                g_in_a = 0; g_cur_link = -1;
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── Preformatted text ── */
            if (tag_match(html, i, len, "pre") == 1) {
                in_pre = 1; goto skip_tag;
            }
            if (tag_match_exact(html, i, len, "pre") == 2) {
                flush_buf(buf, &col); add_blank_line();
                in_pre = 0;
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── Forms ── */
            if (tag_match(html, i, len, "form") == 1) {
                if (ow_form_cnt < OW_MAX_FORMS) {
                    ow_form_t *f = &ow_forms[ow_form_cnt];
                    memset(f, 0, sizeof(ow_form_t));
                    f->field_start = (uint8_t)ow_field_cnt;
                    ow_attr_t at[8];
                    int na = parse_attrs(html, len, i + 1, at, 8);
                    for (int k = 0; k < na; k++) {
                        if (strcmp(at[k].name, "action") == 0)
                            ow_strncpy_n(f->action, at[k].value, OW_URL_MAX);
                        else if (strcmp(at[k].name, "method") == 0 &&
                                 (at[k].value[0] == 'p' || at[k].value[0] == 'P'))
                            f->method = OW_FM_POST;
                    }
                    g_cur_form = ow_form_cnt;
                    ow_form_cnt++;
                    flush_buf(buf, &col); add_blank_line();
                } else g_cur_form = -1;
                goto skip_tag;
            }
            if (tag_match(html, i, len, "form") == 2) {
                if (g_cur_form >= 0 && g_cur_form < ow_form_cnt)
                    ow_forms[g_cur_form].field_count =
                        (uint8_t)(ow_field_cnt - ow_forms[g_cur_form].field_start);
                g_cur_form = -1;
                flush_buf(buf, &col); add_blank_line();
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* <input> */
            if (tag_match(html, i, len, "input") == 1) {
                ow_attr_t at[12];
                int na = parse_attrs(html, len, i + 1, at, 12);
                char fname[OW_URL_MAX]; fname[0] = 0;
                char fval[OW_URL_MAX];  fval[0] = 0;
                char ftype[16];         ftype[0] = 0;
                int checked = 0;
                for (int k = 0; k < na; k++) {
                    if (strcmp(at[k].name, "name") == 0)
                        ow_strncpy_n(fname, at[k].value, OW_URL_MAX);
                    else if (strcmp(at[k].name, "value") == 0)
                        ow_strncpy_n(fval, at[k].value, OW_URL_MAX);
                    else if (strcmp(at[k].name, "type") == 0) {
                        int t = 0;
                        while (at[k].value[t] && t < 15) {
                            ftype[t] = lower_ascii(at[k].value[t]); t++;
                        }
                        ftype[t] = 0;
                    }
                    else if (strcmp(at[k].name, "checked") == 0) checked = 1;
                }
                int tid = field_type_id(ftype);
                if (tid >= 0 && ow_field_cnt < OW_MAX_FIELDS) {
                    ow_form_field_t *f = &ow_form_fields[ow_field_cnt];
                    memset(f, 0, sizeof(ow_form_field_t));
                    f->type = (uint8_t)tid;
                    f->form = g_cur_form;
                    ow_strncpy_n(f->name, fname, OW_URL_MAX);
                    ow_strncpy_n(f->value, fval, OW_URL_MAX);
                    f->checked = checked ? 1 : 0;
                    int fi = ow_field_cnt;
                    ow_field_cnt++;
                    fv_restore(f, fi);
                    render_field_box(f, buf, &col);
                    fv_store(f, fi);
                }
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* <button type=submit|button> */
            if (tag_match(html, i, len, "button") == 1) {
                ow_attr_t at[8];
                int na = parse_attrs(html, len, i + 1, at, 8);
                char fname[OW_URL_MAX]; fname[0] = 0;
                char fval[OW_URL_MAX];  fval[0] = 0;
                int is_submit = 1;
                for (int k = 0; k < na; k++) {
                    if (strcmp(at[k].name, "name") == 0)
                        ow_strncpy_n(fname, at[k].value, OW_URL_MAX);
                    else if (strcmp(at[k].name, "value") == 0)
                        ow_strncpy_n(fval, at[k].value, OW_URL_MAX);
                    else if (strcmp(at[k].name, "type") == 0 &&
                             strcmp(at[k].value, "reset") == 0)
                        is_submit = -1;                  /* reset: ignore */
                    else if (strcmp(at[k].name, "type") == 0 &&
                             strcmp(at[k].value, "submit") != 0)
                        is_submit = 0;                   /* plain button */
                }
                if (is_submit >= 0 && ow_field_cnt < OW_MAX_FIELDS) {
                    ow_form_field_t *f = &ow_form_fields[ow_field_cnt];
                    memset(f, 0, sizeof(ow_form_field_t));
                    f->type = (uint8_t)(is_submit ? OW_FT_SUBMIT : OW_FT_BUTTON);
                    f->form = g_cur_form;
                    ow_strncpy_n(f->name, fname, OW_URL_MAX);
                    ow_strncpy_n(f->value, fval[0] ? fval : "Submit", OW_URL_MAX);
                    int fi = ow_field_cnt;
                    ow_field_cnt++;
                    fv_restore(f, fi);
                    render_field_box(f, buf, &col);
                    fv_store(f, fi);
                }
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* <textarea> (value captured until </textarea>) */
            if (tag_match(html, i, len, "textarea") == 1) {
                g_textarea_fi = ow_field_cnt;
                if (g_textarea_fi < OW_MAX_FIELDS) {
                    ow_form_field_t *f = &ow_form_fields[g_textarea_fi];
                    memset(f, 0, sizeof(ow_form_field_t));
                    f->type = OW_FT_TEXTAREA;
                    f->form = g_cur_form;
                    ow_attr_t at[8];
                    int na = parse_attrs(html, len, i + 1, at, 8);
                    for (int k = 0; k < na; k++)
                        if (strcmp(at[k].name, "name") == 0)
                            ow_strncpy_n(f->name, at[k].value, OW_URL_MAX);
                    ow_field_cnt++;
                } else g_textarea_fi = -1;
                goto skip_tag;
            }
            if (tag_match(html, i, len, "textarea") == 2) {
                int fi = g_textarea_fi;
                if (fi >= 0) {
                    ow_form_field_t *f = &ow_form_fields[fi];
                    int l = (int)strlen(f->value);
                    while (l > 0 && (f->value[l-1] == ' ' || f->value[l-1] == '\n' ||
                                     f->value[l-1] == '\r' || f->value[l-1] == '\t'))
                        f->value[--l] = 0;
                    fv_restore(f, fi);
                    render_textarea_box(f, buf, &col);
                    fv_store(f, fi);
                }
                g_textarea_fi = -1;
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* <select>/<option> */
            if (tag_match(html, i, len, "select") == 1) {
                g_select_fi = ow_field_cnt;
                if (g_select_fi < OW_MAX_FIELDS) {
                    ow_form_field_t *f = &ow_form_fields[g_select_fi];
                    memset(f, 0, sizeof(ow_form_field_t));
                    f->type = OW_FT_SELECT;
                    f->form = g_cur_form;
                    ow_attr_t at[8];
                    int na = parse_attrs(html, len, i + 1, at, 8);
                    for (int k = 0; k < na; k++)
                        if (strcmp(at[k].name, "name") == 0)
                            ow_strncpy_n(f->name, at[k].value, OW_URL_MAX);
                    ow_field_cnt++;
                } else g_select_fi = -1;
                g_in_option = 0;
                goto skip_tag;
            }
            if (tag_match(html, i, len, "option") == 1) {
                if (g_select_fi >= 0) {
                    ow_attr_t at[4];
                    int na = parse_attrs(html, len, i + 1, at, 4);
                    g_opt_sel = 0;
                    for (int k = 0; k < na; k++)
                        if (strcmp(at[k].name, "selected") == 0) g_opt_sel = 1;
                    g_opt_tmp[0] = 0;
                    g_in_option = 1;
                }
                goto skip_tag;
            }
            if (tag_match(html, i, len, "option") == 2) {
                if (g_select_fi >= 0 && g_in_option) {
                    g_in_option = 0;
                    int l = (int)strlen(g_opt_tmp);
                    while (l > 0 && (g_opt_tmp[l-1] == ' ' || g_opt_tmp[l-1] == '\t'))
                        g_opt_tmp[--l] = 0;
                    if (g_opt_tmp[0])
                        opt_add(&ow_form_fields[g_select_fi], g_opt_tmp, g_opt_sel);
                    g_opt_sel = 0;
                }
                i = skip_tag_end(html, len, i);
                continue;
            }
            if (tag_match(html, i, len, "select") == 2) {
                int fi = g_select_fi;
                if (fi >= 0) {
                    ow_form_field_t *f = &ow_form_fields[fi];
                    fv_restore(f, fi);
                    render_field_box(f, buf, &col);
                    fv_store(f, fi);
                }
                g_select_fi = -1; g_in_option = 0;
                i = skip_tag_end(html, len, i);
                continue;
            }

            /* ── Skip <script>..</script>, <svg>..</svg>, <style> and other
                 containers entirely — their text is not site content ── */
            {
                int skiptag = 0;
                for (size_t st = 0; st < sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]); st++) {
                    if (tag_match(html, i, len, SKIP_TAGS[st]) == 1) {
                        skiptag = 1;
                        int clen = (int)strlen(SKIP_TAGS[st]);
                        const char *close_tag = SKIP_TAGS[st];
                        int budget = len - i;        /* never scan past end */
                        while (i < len && budget >= 0) {
                            if (i + 2 + clen <= len && html[i] == '<' && html[i+1] == '/' &&
                                strncmp(html + i + 2, close_tag, (size_t)clen) == 0) {
                                i = skip_tag_end(html, len, i);
                                break;
                            }
                            i++;
                            budget--;
                        }
                        break;
                    }
                }
                if (skiptag) continue;
            }

            /* skip all other tags */
skip_tag:
            i = skip_tag_end(html, len, i);
            continue;
        }

        /* ── form control value capture (no grid output yet) ── */
        if (g_in_option) {
            int ot = (int)strlen(g_opt_tmp);
            if (html[i] != '<' && html[i] >= ' ' && ot < OW_SELECT_OPT_SZ - 1) {
                g_opt_tmp[ot] = html[i]; g_opt_tmp[ot + 1] = 0;
            }
            i++;
            continue;
        }
        if (g_select_fi >= 0) {   /* swallow stray select text */
            i++;
            continue;
        }
        if (g_textarea_fi >= 0) {
            if (html[i] != '<') {
                ow_form_field_t *f = &ow_form_fields[g_textarea_fi];
                int vl = (int)strlen(f->value);
                if (vl < OW_URL_MAX - 1) { f->value[vl] = html[i]; f->value[vl + 1] = 0; }
            }
            i++;
            continue;
        }

        /* ── HTML entities (named + numeric) ── */
        if (html[i] == '&') {
            unsigned char eout[5];
            int el;
            int consumed = try_entity(html, len, i, eout, &el);
            if (consumed > 0) {
                buf_put(buf, &col, (const char *)eout, el);
                i += consumed;
                continue;
            }
        }

        /* ── Whitespace handling in pre mode ── */
        if (in_title) {
            if (html[i] >= ' ' && html[i] != '<') {
                int plen = (int)strlen(ow_page_title);
                if (plen < OW_URL_MAX - 1) { ow_page_title[plen] = html[i]; ow_page_title[plen + 1] = 0; }
            }
            i++;
            continue;
        }
        if (in_pre) {
            if (html[i] == '\n') {
                flush_buf(buf, &col);
                i++;
                continue;
            }
            if (html[i] >= ' ') {
                if (col < OW_TXT_COLS - 1) buf[col++] = html[i];
                i++;
                continue;
            }
            i++;
            continue;
        }

        if (html[i] == '\r') { i++; continue; }
        if (html[i] == '\n' || html[i] == '\t') {
            if (in_row) {
                if (col < OW_TXT_COLS - 1) buf[col++] = ' ';
                i++;
                continue;
            }
            if (html[i] == '\t') { if(col<OW_TXT_COLS-1) buf[col++]=' '; if(col<OW_TXT_COLS-1) buf[col++]=' '; i++; continue; }
            int para = (i > 0 && html[i-1] == '\n');
            if (col > 0 || para) {
                flush_buf(buf, &col);
            }
            i++; continue;
        }

        if (html[i] >= ' ') {
            if (html[i] == ' ') {
                while (i + 1 < len && html[i+1] == ' ') i++;
            }
            if (col >= OW_TXT_COLS - 1) {
                int brk = -1;
                for (int k = col-1; k >= 0; k--) { if (buf[k] == ' ') { brk = k; break; } }
                if (brk > 0) {
                    flush_buf(buf, &brk);
                    int ni = 0;
                    for (int k = brk + 1; k < col; k++) buf[ni++] = buf[k];
                    col = ni;
                } else {
                    flush_buf(buf, &col);
                    col = 0;
                }
            }
            if (g_in_a && g_cur_link >= 0 && g_cur_link < OW_MAX_LINKS) {
                if (ow_links[g_cur_link].line < 0) { ow_links[g_cur_link].line = ow_txt_lines; ow_links[g_cur_link].sc = col; }
            }
            if (g_in_bold) g_line_bold = 1;
            if (g_in_italic) g_line_italic = 1;
            if (g_in_code) g_line_code = 1;
            buf[col++] = html[i];
            if (g_in_a && g_cur_link >= 0 && g_cur_link < OW_MAX_LINKS) {
                ow_links[g_cur_link].ec = col;
            }
        }
        i++;
    }
    if (col > 0) {
        flush_buf(buf, &col);
    }
    if (g_cur_form >= 0 && g_cur_form < ow_form_cnt)
        ow_forms[g_cur_form].field_count =
            (uint8_t)(ow_field_cnt - ow_forms[g_cur_form].field_start);
    ow_need_render = 0;
}

/* Argmax helper: last byte position equal to 'c' (skip '//' in scheme). */
static int find_slash(const char *s) {
    int i = 0;
    while (s[i] && s[i] != '/') i++;
    return i;
}

/* Fetch an image over plain HTTP(S). `url` must be absolute
 * (http[s]://host[:port]/path). Returns the decoded body length (>0) or -1.
 * The HTTP header frame is stripped; max_len bounds the whole response. */
int ow_image_download(const char *url, void *buf, int max_len) {
    char host[129], path[600];
    if (!url || !url[0] || !buf || max_len <= 8) return -1;

    int https = (strncmp(url, "https://", 8) == 0);
    const char *rest = url;
    if (strncmp(rest, "http://", 7) == 0) rest += 7;
    else if (strncmp(rest, "https://", 8) == 0) rest += 8;
    else return -1;

    int sl = find_slash(rest);
    int hl = sl;
    if (hl > 128) hl = 128;
    for (int i = 0; i < hl; i++) host[i] = rest[i];
    host[hl] = 0;

    uint16_t port = https ? 443 : 80;
    char *colon = strchr(host, ':');
    if (colon) {
        unsigned long p = 0;
        int ok = 1;
        const char *ps = colon + 1;
        for (; *ps; ps++) {
            if (*ps < '0' || *ps > '9') { ok = 0; break; }
            p = p * 10 + (unsigned long)(*ps - '0');
        }
        if (ok) port = (uint16_t)p;
        *colon = 0;
    }

    if (rest[sl]) {
        int pl = 0;
        for (int i = sl; rest[i] && i < sl + 590; i++) path[pl++] = rest[i];
        path[pl] = 0;
    } else {
        path[0] = '/'; path[1] = 0;
    }

    int cap = (max_len < 65535) ? max_len : 65535;
    int n = https ? https_get(host, port, path, buf, (uint16_t)cap)
                  : http_get(host, port, path, buf, (uint16_t)cap);
    if (n <= 0) return -1;

    /* Strip the header frame (first \r\n\r\n). */
    unsigned char *b = (unsigned char *)buf;
    for (int i = 0; i + 3 < n; i++) {
        if (b[i] == '\r' && b[i+1] == '\n' && b[i+2] == '\r' && b[i+3] == '\n') {
            int body = n - (i + 4);
            memmove(b, b + i + 4, (size_t)body);
            return body;
        }
    }
    /* No header found — treat response as raw body. */
    return n;
}