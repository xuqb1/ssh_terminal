#include "my_terminal.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "cui_internal.h"

#define MY__TM_TEXT    0
#define MY__TM_ESC     1
#define MY__TM_CSI     2
#define MY__TM_OSC     3
#define MY__TM_OSC_ESC 4

/* ================================================================== */
/* 滚动位置                                                            */
/* ================================================================== */
#define MY_MAX_TERMS 64

static struct { cui_id id; double top; bool follow; } g_my_scroll[MY_MAX_TERMS];
static int g_my_scroll_count = 0;


static bool g_term_ctx_open = false;
static float g_term_ctx_x = 0, g_term_ctx_y = 0;
static int   g_term_ctx_tab = -1;   /* 哪个终端右键的 */

//extern char lang;
#define T(zh, en) (lang == 'z' ? (zh) : (en))

static double *my__get_scroll(cui_id id, bool *follow) {
    for (int i = 0; i < g_my_scroll_count; i++)
        if (g_my_scroll[i].id == id) {
            if (follow) *follow = g_my_scroll[i].follow;
            return &g_my_scroll[i].top;
        }
    if (g_my_scroll_count >= MY_MAX_TERMS) return NULL;
    g_my_scroll[g_my_scroll_count].id = id;
    g_my_scroll[g_my_scroll_count].top = 0;
    g_my_scroll[g_my_scroll_count].follow = true;
    if (follow) *follow = true;
    return &g_my_scroll[g_my_scroll_count++].top;
}

static void my__set_follow(cui_id id, bool follow) {
    for (int i = 0; i < g_my_scroll_count; i++)
        if (g_my_scroll[i].id == id) { g_my_scroll[i].follow = follow; return; }
}

/* ================================================================== */
/* 选区                                                                */
/* ================================================================== */
typedef struct { int line; int col; } my__pos;

typedef struct {
    cui_id id;
    bool selecting, has_sel;
    my__pos a, b;
    float press_x, press_y;
} my__sel_state;

static my__sel_state g_my_sel[MY_MAX_TERMS];
static int g_my_sel_count = 0;

static my__sel_state *my__get_sel(cui_id id) {
    for (int i = 0; i < g_my_sel_count; i++)
        if (g_my_sel[i].id == id) return &g_my_sel[i];
    if (g_my_sel_count >= MY_MAX_TERMS) return NULL;
    memset(&g_my_sel[g_my_sel_count], 0, sizeof g_my_sel[0]);
    g_my_sel[g_my_sel_count].id = id;
    return &g_my_sel[g_my_sel_count++];
}

/* ================================================================== */
/* 终端样式 + SGR 解析                                                */
/* ================================================================== */
typedef struct {
    cui_color fg, bg;
    bool has_fg, has_bg, bold, dim, italic, underline, inverse;
} my__tm_style;

static cui_color my__tm_palette(cui_ctx *ui, int k) {
    static const uint32_t light[16] = { 0x27272a, 0xc4302b, 0x2e7d32, 0x9a6700, 0x1f5fbf, 0x8e3fb0, 0x0f7c84, 0x71717a,
                                        0x52525b, 0xe5484d, 0x30a46c, 0xb08300, 0x3e7bfa, 0xab4aba, 0x12a4ad, 0x18181b };
    static const uint32_t dark[16] = { 0x52525b, 0xff6b6b, 0x5bd18b, 0xf5c451, 0x6aa8ff, 0xd38cf2, 0x4fd1d9, 0xd4d4d8,
                                       0x71717a, 0xff8f8f, 0x7ee2a8, 0xffd97a, 0x93c0ff, 0xe2adf7, 0x7fe3e9, 0xfafafa };
    uint32_t v = (ui->mode == CUI_THEME_DARK ? dark : light)[k & 15];
    return cui__rgba((v >> 16) & 255, (v >> 8) & 255, v & 255, 255);
}

static cui_color my__tm_256(cui_ctx *ui, int n) {
    if (n < 16) return my__tm_palette(ui, n);
    if (n < 232) {
        n -= 16;
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        return cui__rgba(lv[n / 36], lv[n / 6 % 6], lv[n % 6], 255);
    }
    int g = 8 + (n - 232) * 10;
    return cui__rgba(g, g, g, 255);
}

static void my__tm_sgr(cui_ctx *ui, my__tm_style *st, const char *s, int n) {
    int p[16], np = 0, v = -1;
    for (int k = 2; k < n - 1 && np < 16; k++) {
        char c = s[k];
        if (c >= '0' && c <= '9') v = (v < 0 ? 0 : v) * 10 + (c - '0');
        else if (c == ';' || c == ':') { p[np++] = v < 0 ? 0 : v; v = -1; }
    }
    if (np < 16) p[np++] = v < 0 ? 0 : v;
    for (int k = 0; k < np; k++) {
        int c = p[k];
        if (c == 0) { memset(st, 0, sizeof *st); }
        else if (c == 1) st->bold = true;
        else if (c == 2) st->dim = true;
        else if (c == 3) st->italic = true;
        else if (c == 4) st->underline = true;
        else if (c == 7) st->inverse = true;
        else if (c == 22) st->bold = st->dim = false;
        else if (c == 23) st->italic = false;
        else if (c == 24) st->underline = false;
        else if (c == 27) st->inverse = false;
        else if (c >= 30 && c <= 37) { st->fg = my__tm_palette(ui, c - 30); st->has_fg = true; }
        else if (c >= 90 && c <= 97) { st->fg = my__tm_palette(ui, c - 90 + 8); st->has_fg = true; }
        else if (c >= 40 && c <= 47) { st->bg = my__tm_palette(ui, c - 40); st->has_bg = true; }
        else if (c >= 100 && c <= 107) { st->bg = my__tm_palette(ui, c - 100 + 8); st->has_bg = true; }
        else if (c == 39) st->has_fg = false;
        else if (c == 49) st->has_bg = false;
        else if ((c == 38 || c == 48) && k + 1 < np) {
            cui_color col;
            if (p[k + 1] == 5 && k + 2 < np) { col = my__tm_256(ui, p[k + 2] & 255); k += 2; }
            else if (p[k + 1] == 2 && k + 4 < np) { col = cui__rgba(p[k + 2] & 255, p[k + 3] & 255, p[k + 4] & 255, 255); k += 4; }
            else continue;
            if (c == 38) { st->fg = col; st->has_fg = true; } else { st->bg = col; st->has_bg = true; }
        }
    }
}

/* ================================================================== */
/* Buffer                                                              */
/* ================================================================== */

static cui_terminal_line *my__tm_cur(cui_terminal_buffer *t) {
    return &t->lines[(t->first + t->count - 1) % t->line_cap];
}
static cui_terminal_line *my__tm_line(const cui_terminal_buffer *t, int i) {
    return &t->lines[(t->first + i) % t->line_cap];
}

void my_terminal_init(cui_terminal_buffer *t, char *text, size_t text_cap,
                      cui_terminal_line *lines, int line_cap) {
    if (!t) return;
    memset(t, 0, sizeof *t);
    t->text = text;
    t->text_cap = (uint32_t)CUI_MIN(text_cap, (size_t)0x7FFFFFFF);
    t->lines = lines;
    t->line_cap = line_cap;
    if (!text || !lines || line_cap < 2 || t->text_cap < 64) { t->line_cap = 0; return; }
    t->count = 1;
    t->total = 1;
    lines[0] = (cui_terminal_line){ 0, 0, 0 };
}

void my_terminal_clear(cui_terminal_buffer *t) {
    if (!t || !t->line_cap) return;
    t->first = 0; t->count = 1; t->end = 0; t->total++;
    t->lines[0] = (cui_terminal_line){ 0, 0, 0 };
    t->cr = false; t->mode = MY__TM_TEXT;
}

static void my__tm_new_line(cui_terminal_buffer *t) {
    if (t->count == t->line_cap) { t->first = (t->first + 1) % t->line_cap; t->count--; }
    t->lines[(t->first + t->count) % t->line_cap] = (cui_terminal_line){ t->end, 0, 0 };
    t->count++; t->total++;
}

static bool my__tm_room(cui_terminal_buffer *t, uint32_t n) {
    cui_terminal_line *c = my__tm_cur(t);
    if (c->len + n > t->text_cap) return false;
    if (c->offset + c->len + n > t->text_cap) {
        uint32_t need = c->len + n;
        while (t->count > 1) {
            cui_terminal_line *o = my__tm_line(t, 0);
            if (o->offset < need) { t->first = (t->first + 1) % t->line_cap; t->count--; }
            else break;
        }
        memmove(t->text, t->text + c->offset, c->len);
        c->offset = 0;
        t->end = c->len;
    }
    while (t->count > 1) {
        cui_terminal_line *o = my__tm_line(t, 0);
        if (o->offset >= t->end && o->offset < t->end + n) { t->first = (t->first + 1) % t->line_cap; t->count--; }
        else break;
    }
    return true;
}

static void my__tm_put(cui_terminal_buffer *t, const char *b, uint32_t n, uint32_t cols) {
    if (!my__tm_room(t, n)) return;
    memcpy(t->text + t->end, b, n);
    t->end += n;
    cui_terminal_line *c = my__tm_cur(t);
    c->len += n; c->cols += cols;
}

static void my__tm_restart(cui_terminal_buffer *t) {
    cui_terminal_line *c = my__tm_cur(t);
    t->end = c->offset; c->len = 0; c->cols = 0;
    t->cr = false;
}

void my_terminal_write(cui_terminal_buffer *t, const char *data, int len) {
    if (!t || !t->line_cap || !data) return;
    if (len < 0) len = (int)strlen(data);
    int run = -1;
#define MY_FLUSH(upto) do { \
        if (run >= 0) { \
            uint32_t cols_ = 0; \
            for (int k_ = run; k_ < (upto); k_++) cols_ += ((unsigned char)data[k_] & 0xC0) != 0x80; \
            my__tm_put(t, data + run, (uint32_t)((upto) - run), cols_); run = -1; } } while (0)
    for (int i = 0; i < len; i++) {
        unsigned char b = (unsigned char)data[i];
        switch (t->mode) {
        case MY__TM_TEXT:
            if (b == 0x1b) { MY_FLUSH(i); t->mode = MY__TM_ESC; t->seq_n = 0; t->seq[t->seq_n++] = (char)b; }
            else if (b == '\n') { MY_FLUSH(i); t->cr = false; my__tm_new_line(t); }
            else if (b == '\r') { MY_FLUSH(i); t->cr = true; }
            else if (b == '\t') {
                MY_FLUSH(i);
                if (t->cr) my__tm_restart(t);
                uint32_t sp = 8 - my__tm_cur(t)->cols % 8;
                my__tm_put(t, "        ", sp, sp);
            } else if (b == '\b') {
                MY_FLUSH(i);
                cui_terminal_line *c = my__tm_cur(t);
                if (c->len > 0 && c->len == c->cols) { c->len--; c->cols--; t->end--; }
            } else if (b < 0x20 || b == 0x7f) { MY_FLUSH(i); }
            else {
                if (t->cr) { MY_FLUSH(i); my__tm_restart(t); }
                if (run < 0) run = i;
            }
            break;
        case MY__TM_ESC:
            if (b == '[') { t->mode = MY__TM_CSI; if (t->seq_n < sizeof t->seq) t->seq[t->seq_n++] = (char)b; }
            else if (b == ']') t->mode = MY__TM_OSC;
            else t->mode = MY__TM_TEXT;
            break;
        case MY__TM_CSI:
            if (t->seq_n < sizeof t->seq) t->seq[t->seq_n++] = (char)b;
            if (b >= 0x40 && b <= 0x7e) {
                t->mode = MY__TM_TEXT;
                if (b == 'm') {
                    if (t->cr) my__tm_restart(t);
                    my__tm_put(t, t->seq, t->seq_n, 0);
                } else if (b == 'K' && t->cr) my__tm_restart(t);
                else if (b == 'J' && memchr(t->seq, '2', t->seq_n)) my_terminal_clear(t);
            }
            break;
        case MY__TM_OSC:
            if (b == 0x07) t->mode = MY__TM_TEXT;
            else if (b == 0x1b) t->mode = MY__TM_OSC_ESC;
            break;
        case MY__TM_OSC_ESC:
            t->mode = b == '\\' ? MY__TM_TEXT : MY__TM_OSC;
            break;
        }
    }
    MY_FLUSH(len);
#undef MY_FLUSH
}

void my_terminal_printf(cui_terminal_buffer *t, const char *fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) my_terminal_write(t, buf, CUI_MIN(n, (int)sizeof buf - 1));
}

/* ================================================================== */
/* 渲染                                                                */
/* ================================================================== */
typedef struct {
    cui_terminal_buffer *t;
    float size;
    int cols;
    float cw, lh;
    cui_color fg, sel, bar;
    float bar_t;
    double rows_total;
    cui_id wheel;
    bool focused;
    double top;
    bool has_sel;
    my__pos s0, s1;
} my__paint;

static int my__rows(uint32_t cols, int width_cols) {
    return cols == 0 ? 1 : (int)((cols + (uint32_t)width_cols - 1) / (uint32_t)width_cols);
}

static void my__hit_test(cui_ctx *ui, cui_rect r, my__paint *p,
                         float mx, float my, int *line_out, int *col_out) {
    float pad = 10, x0 = r.x + pad, y0 = r.y + 6;
    (void)ui;
    double row_hit = p->top + (my - y0) / p->lh;
    if (row_hit < 0) row_hit = 0;
    double row_acc = 0;
    int li = 0;
    while (li < p->t->count) {
        int rr = my__rows(my__tm_line(p->t, li)->cols, p->cols);
        if (row_acc + rr > row_hit) break;
        row_acc += rr; li++;
    }
    if (li >= p->t->count) li = p->t->count - 1;
    int sub = (int)(row_hit - row_acc);
    int cc = (int)floorf((mx - x0) / p->cw + 0.5f);
    if (cc < 0) cc = 0;
    if (cc > p->cols) cc = p->cols;
    *line_out = li;
    *col_out = sub * p->cols + cc;
}

static void my__paint_terminal(cui_ctx *ui, cui_node *n, void *data) {
    my__paint *p = data;
    cui_rect r = n->rect;
    cui__paint_wheel_hit(ui, p->wheel, r);
    const cui_terminal_buffer *t = p->t;
    if (!t || !t->line_cap) return;

    float pad = 10, x0 = r.x + pad, y0 = r.y + 6;
    float fh = cui__font_line_height(ui, CUI_FONT_MONO, p->size);
    double row = 0;
    int li = 0;
    long long want = (long long)floor(p->top);
    while (li < t->count) {
        int rr = my__rows(my__tm_line(t, li)->cols, p->cols);
        if (row + rr > want) break;
        row += rr; li++;
    }
    float y = y0 + (float)(row - p->top) * p->lh;
    float first_y = y0 + (float)(floor(p->top) - p->top) * p->lh;
    float clip_top = first_y < y0 - 0.5f ? first_y + p->lh : r.y + 3;
    cui_push_clip(ui, (cui_rect){ r.x, clip_top, r.w, r.y + r.h - 3 - clip_top });

    /* 选区高亮 */
    if (p->has_sel) {
        my__pos s0 = p->s0, s1 = p->s1;
        if (s1.line < s0.line || (s1.line == s0.line && s1.col < s0.col)) {
            my__pos tmp = s0; s0 = s1; s1 = tmp;
        }
        for (int k = s0.line; k <= s1.line && k < t->count; k++) {
            const cui_terminal_line *L = my__tm_line(t, k);
            int c0 = (k == s0.line) ? s0.col : 0;
            int c1 = (k == s1.line) ? s1.col : (int)L->cols + 1;
            if (c1 <= c0) continue;
            double row_of_line = 0;
            for (int m = 0; m < k; m++)
                row_of_line += my__rows(my__tm_line(t, m)->cols, p->cols);
            float ly = y0 + (float)(row_of_line - p->top) * p->lh;
            for (int c = c0; c < c1;) {
                int rowi = c / p->cols;
                int end = (rowi + 1) * p->cols;
                if (end > c1) end = c1;
                float sy = ly + rowi * p->lh;
                float sx = x0 + (c % p->cols) * p->cw;
                cui_draw_rect(ui, (cui_rect){ sx, sy, (end - c) * p->cw, p->lh },
                              CUI_CORNERS(0), p->sel);
                c = end;
            }
        }
    }

    int last_li = -1;
    for (; li < t->count && y < r.y + r.h; li++) {
        const cui_terminal_line *L = my__tm_line(t, li);
        int nrows = my__rows(L->cols, p->cols);
        last_li = li;

        my__tm_style st; memset(&st, 0, sizeof st);
        const char *s = t->text + L->offset;
        uint32_t i = 0, col = 0;
        while (i < L->len) {
            if ((unsigned char)s[i] == 0x1b) {
                uint32_t j = i + 1;
                while (j < L->len && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e && j > i + 1)) j++;
                if (j < L->len) my__tm_sgr(ui, &st, s + i, (int)(j - i + 1));
                i = j + 1;
                continue;
            }
            uint32_t j = i, c = col, room = (uint32_t)p->cols - col % (uint32_t)p->cols;
            uint32_t taken = 0;
            while (j < L->len && (unsigned char)s[j] != 0x1b) {
                bool lead = ((unsigned char)s[j] & 0xC0) != 0x80;
                if (lead) { if (taken == room) break; taken++; }
                j++;
            }
            cui_color fg = st.has_fg ? st.fg : p->fg;
            cui_color bg = st.has_bg ? st.bg : cui__rgba(0, 0, 0, 0);
            if (st.inverse) { cui_color tmp = fg; fg = st.has_bg ? st.bg : CUI_C(ui, SURFACE); bg = tmp; }
            if (st.dim) fg = cui_alpha(fg, 0.6f);
            float rx = x0 + (c % (uint32_t)p->cols) * p->cw, ry = y + (c / (uint32_t)p->cols) * p->lh;
            if (ry > r.y + r.h) break;
            if (bg.a) cui_draw_rect(ui, (cui_rect){ rx, ry, taken * p->cw, p->lh }, CUI_CORNERS(0), bg);
            if (ry + p->lh >= r.y) {
                uint32_t k = i;
                float cx = rx;
                while (k < j) {
                    uint32_t e = k + 1;
                    while (e < j && ((unsigned char)s[e] & 0xC0) == 0x80) e++;
                    if (s[k] != ' ')
                        cui__draw_text_line(ui, CUI_FONT_MONO, p->size, cx,
                                            ry + (p->lh - fh) * 0.5f, fg, s + k, (int)(e - k), true);
                    k = e; cx += p->cw;
                }
            }
            col += taken; i = j;
        }
        y += nrows * p->lh;
    }
    cui_pop_clip(ui);

    /* 光标：跟随最后一行末尾（shell 回显末尾） */
    if (p->focused && last_li >= 0) {
        const cui_terminal_line *L = my__tm_line(t, last_li);
        int cur = (int)L->len;
        const char *s = t->text + L->offset;
        int cols_total = 0;
        uint32_t i = 0;
        while (i < (uint32_t)cur && i < L->len) {
            if ((unsigned char)s[i] == 0x1b) {
                uint32_t j = i + 1;
                while (j < L->len && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e && j > i + 1)) j++;
                i = j + 1;
            } else {
                cols_total++;
                i++;
            }
        }
        double row_start = 0;
        for (int k = 0; k < last_li; k++)
            row_start += my__rows(my__tm_line(t, k)->cols, p->cols);
        float ly = y0 + (float)(row_start - p->top) * p->lh;
        int cur_row = cols_total / p->cols;
        int cur_col = cols_total % p->cols;
        double now = cui_time(ui);
        if (fmod(now, 1.0) < 0.5) {
            float cx = x0 + cur_col * p->cw;
            float cy = ly + cur_row * p->lh;
            cui_draw_rect(ui, (cui_rect){ cx, cy + 2, 2, p->lh - 4 },
                          CUI_CORNERS(0), p->fg);
        }
    }

    /* 滚动条 */
    if (p->bar_t > 0.01f && p->rows_total * p->lh > r.h) {
        float vh = r.h, th = CUI_MAX(24.0f, vh * vh / (float)(p->rows_total * p->lh));
        float maxtop = (float)(p->rows_total - (r.h - 12) / p->lh);
        float ty = r.y + (maxtop > 0 ? (float)(p->top / maxtop) : 1) * (vh - th);
        cui_draw_rect(ui, (cui_rect){ r.x + r.w - 6, ty, 3, th },
                      CUI_CORNERS(1.5f), cui_alpha(p->bar, p->bar_t));
    }
}

/* ================================================================== */
/* 选区文本提取                                                       */
/* ================================================================== */
static int my__copy_selection(const cui_terminal_buffer *t, my__pos a, my__pos b,
                              char *out, int cap) {
    int n = 0;
    if (b.line < a.line || (b.line == a.line && b.col < a.col)) {
        my__pos tmp = a; a = b; b = tmp;
    }
    for (int k = a.line; k <= b.line && k < t->count; k++) {
        const cui_terminal_line *L = my__tm_line(t, k);
        int c0 = (k == a.line) ? a.col : 0;
        int c1 = (k == b.line) ? b.col : (int)L->cols + 1;
        const char *s = t->text + L->offset;
        int col = 0;
        for (uint32_t i = 0; i < L->len;) {
            if ((unsigned char)s[i] == 0x1b) {
                uint32_t j = i + 1;
                while (j < L->len && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e && j > i + 1)) j++;
                i = j + 1;
                continue;
            }
            uint32_t e = i + 1;
            while (e < L->len && ((unsigned char)s[e] & 0xC0) == 0x80) e++;
            if (col >= c0 && col < c1 && n + (int)(e - i) < cap - 2) {
                memcpy(out + n, s + i, e - i);
                n += (int)(e - i);
            }
            col++; i = e;
        }
        if (k != b.line && n < cap - 2) out[n++] = '\n';
    }
    out[n] = 0;
    return n;
}

/* ================================================================== */
/* 对外接口                                                           */
/* ================================================================== */
my_terminal_result my_terminal_ex(cui_ctx *ui, const char *id_str,
                                  cui_terminal_buffer *t,
                                  const my_terminal_opts *opts) {
    my_terminal_result res = { NULL, false };
    my_terminal_opts d; memset(&d, 0, sizeof d);
    d.lang = 'z';
    if (!opts) opts = &d;
                
    cui_id id = cui_make_id(ui, id_str ? id_str : "my-term");
    cui_id vid = id ^ 0x7E2Du, wid = id ^ 0x7E2Eu;

    float size = opts->size > 0 ? opts->size : 13;
    float cw = cui__text_width(ui, CUI_FONT_MONO, size, "0", 1, true);
    if (cw <= 0) cw = size * 0.6f;
    float lh = ceilf(size * 1.45f);

    cui_rect pr = cui__prev_rect(ui, vid);
    float vw = pr.w > 0 ? pr.w : 600;
    int cols = CUI_MAX(8, (int)((vw - 20) / cw));

    bool ok = t && t->line_cap;
    double rows_total = 0;
    if (ok) for (int i = 0; i < t->count; i++)
        rows_total += my__rows(my__tm_line(t, i)->cols, cols);

    float h = opts->height > 0 ? opts->height : 320;

    /* 滚动位置 */
    bool follow = true;
    double *top_ptr = my__get_scroll(vid, &follow);
    double page = (h - 12) / lh;
    double maxtop = rows_total > page ? rows_total - page : 0;

    /* 滚轮：用上一帧的 scroll_target */
    if (ui->scroll_target == wid && ui->scroll_dy != 0) {
        follow = false;
        my__set_follow(vid, false);
        double tt = *top_ptr - ui->scroll_dy * 3;
        if (tt < 0) tt = 0;                          /* 到顶：钳住 0，不恢复 follow */
        if (tt >= maxtop) { tt = maxtop; follow = true; my__set_follow(vid, true); }  /* 到底：恢复 follow */
        *top_ptr = CUI_CLAMP(tt, 0, maxtop);
        ui->scroll_dy = 0;
    }
    if (follow) *top_ptr = maxtop;
    *top_ptr = CUI_CLAMP(*top_ptr, 0, maxtop);

    int col = cui__node_new(ui, id);
    cui_node *cn = &ui->nodes[col];
    cn->w = opts->width.kind ? opts->width : CUI_FILL;
    cn->h = opts->height < 0 ? CUI_FILL : CUI_PX(h);
    cn->self_align = CUI_ALIGN_STRETCH; cn->gap = 0;
    cn->bg = ui->mode == CUI_THEME_DARK ? cui_mix(CUI_C(ui, BACKGROUND), CUI_C(ui, SURFACE), 0.5f) : CUI_C(ui, SURFACE);
    cn->border = 1; cn->border_color = CUI_C(ui, BORDER);
    cn->radius = CUI_CORNERS(ui->theme.radius_control);
    cn->flags |= CUI_NF_CLIP;
    cui__open(ui, col);

    int v = cui__leaf(ui, vid, CUI_FILL, CUI_FILL);
    ui->nodes[v].self_align = CUI_ALIGN_STRETCH;
    cui__mark_interactive(ui, v, 0);

    my__paint *p = (my__paint *)cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->t = ok ? t : NULL;
    p->size = size; p->cols = cols; p->cw = cw; p->lh = lh;
    p->fg = CUI_C(ui, FOREGROUND);
    p->sel = cui_alpha(CUI_C(ui, ACCENT), 0.3f);
    p->bar = CUI_C(ui, BORDER_STRONG);
    p->bar_t = 1; p->rows_total = rows_total;
    p->wheel = wid;
    p->focused = (ui->focus == vid);
    p->top = *top_ptr;
    p->has_sel = false;
    ui->nodes[v].paint = my__paint_terminal;
    ui->nodes[v].paint_data = p;

    /* 交互 */
    cui_interact vr = cui__interact(ui, vid, 0);
    if (vr.pressed) ui->focus = vid;

    /* ===== 右键菜单 ===== */
    my__sel_state *sel = my__get_sel(vid);   /* 先拿到 sel，菜单要用 */

    if (vr.hovered && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT)) {
        g_term_ctx_open = true;
        g_term_ctx_x = ui->mx;
        g_term_ctx_y = ui->my;
        g_term_ctx_tab = (int)vid;   /* 用 vid 区分不同终端 */
    }

    if (g_term_ctx_open && g_term_ctx_tab == (int)vid) {
        float item_h = 28;
        float menu_w = 120;
        float menu_h = item_h * 4 + 8;
        float mx = g_term_ctx_x, my = g_term_ctx_y;
        float vw = cui_viewport_size(ui).x, vh = cui_viewport_size(ui).y;
        if (mx + menu_w > vw) mx = vw - menu_w - 4;
        if (my + menu_h > vh) my = vh - menu_h - 4;
        if (mx < 4) mx = 4;
        if (my < 4) my = 4;

        /* 遮罩，拦截外部点击 */
        cui_id backdrop_id = cui_make_id(ui, "term-ctx-backdrop");
        int backdrop_node = cui__float_begin(ui, backdrop_id, 99, 0, CUI__PLACE_POINT, 0);
        ui->nodes[backdrop_node].fx = 0;
        ui->nodes[backdrop_node].fy = 0;
        ui->nodes[backdrop_node].w = CUI_PX(vw);
        ui->nodes[backdrop_node].h = CUI_PX(vh);
        ui->nodes[backdrop_node].flags |= CUI_NF_INTERACTIVE;
        cui__float_end(ui);
        cui_interact bvr = cui__interact(ui, backdrop_id, 0);

        /* 菜单本体 */
        cui_id menu_id = cui_make_id(ui, "term-ctx-menu");
        int menu_node = cui__float_begin(ui, menu_id, 100, 0, CUI__PLACE_POINT, 0);
        ui->nodes[menu_node].fx = mx;
        ui->nodes[menu_node].fy = my;
        ui->nodes[menu_node].w = CUI_PX(menu_w);
        ui->nodes[menu_node].h = CUI_PX(menu_h);
        cui__open(ui, menu_node);
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 4, .gap = 0,
                    .bg = CUI_COLOR_SURFACE_RAISED, .border = 1,
                    .border_color = CUI_COLOR_BORDER_STRONG, .radius = 6) {
                static const char *items_zh[] = { "全选", "复制", "粘贴", "清屏" };
                static const char *items_en[] = { "Select all", "Copy", "Paste", "Clear" };
                const char **items = (opts->lang == 'z') ? items_zh : items_en;
                for (int k = 0; k < 4; k++) {
                    if (cui_button(ui, items[k], .variant = CUI_BUTTON_GHOST,
                                   .full_width = true, .size = CUI_SIZE_SM)) {
                        switch (k) {
                        case 0:   /* 全选 */
                            sel->a = (my__pos){ 0, 0 };
                            sel->b = (my__pos){ t->count - 1,
                                                (int)my__tm_line(t, t->count - 1)->cols };
                            sel->has_sel = true;
                            break;
                        case 1:   /* 复制 */
                            if (sel->has_sel) {
                                static char copybuf[64 * 1024];
                                int n = my__copy_selection(t, sel->a, sel->b,
                                                            copybuf, sizeof copybuf);
                                if (n > 0 && ui->platform.set_clipboard)
                                    ui->platform.set_clipboard(ui->platform.user, copybuf);
                                sel->has_sel = false;
                            }
                            break;
                        case 2:   /* 粘贴 */
                            res.paste = true;
                            break;
                        case 3:   /* 清屏 */
                            res.clear = true;
                            break;
                        }
                        g_term_ctx_open = false;
                    }
                }
            }
        cui__close(ui);
        cui__float_end(ui);

        if (bvr.pressed || cui_key_pressed(ui, CUI_KEY_ESCAPE))
            g_term_ctx_open = false;
    }
    
    //my__sel_state *
    //sel = my__get_sel(vid);

    /* 选区：鼠标按下 / 拖动 / 松开 */
    int hit_line = -1, hit_col = -1;
    if (pr.w > 0) {
        my__hit_test(ui, pr, p, ui->mx, ui->my, &hit_line, &hit_col);
    }

    if (vr.active && pr.w > 0) {
        if (ui->mpressed[0] && hit_line >= 0) {
            sel->a = (my__pos){ hit_line, hit_col };
            sel->b = sel->a;
            sel->selecting = true;
            sel->has_sel = false;
            sel->press_x = ui->mx;
            sel->press_y = ui->my;
        }
        if (sel->selecting && ui->mdown[0] && hit_line >= 0) {
            sel->b = (my__pos){ hit_line, hit_col };
            if (fabsf(ui->mx - sel->press_x) + fabsf(ui->my - sel->press_y) > 3)
                sel->has_sel = true;
        }
        if (sel->selecting && !ui->mdown[0]) {
            sel->selecting = false;
        }
    }

    p->has_sel = sel->has_sel;
    p->s0 = sel->a;
    p->s1 = sel->b;

    /* 键盘：收集 raw_input，不 append 到终端 */
    if (ui->focus == vid && !opts->no_input && ok) {
        static char raw[512];
        int rlen = 0;
        bool shift = (ui->mods & CUI_MOD_SHIFT) != 0;
        bool ctrl  = (ui->mods & CUI_MOD_CTRL)  != 0;

        /* ---------- 1. Ctrl+字母 → 控制码，先统一消费 ---------- */
        static const struct { int key; char ctrl_code; } ctrlmap[] = {
            { CUI_KEY_A, 0x01 }, { CUI_KEY_B, 0x02 }, { CUI_KEY_C, 0x03 },
            { CUI_KEY_D, 0x04 }, { CUI_KEY_E, 0x05 }, { CUI_KEY_F, 0x06 },
            { CUI_KEY_G, 0x07 }, { CUI_KEY_H, 0x08 }, { CUI_KEY_I, 0x09 },
            { CUI_KEY_J, 0x0A }, { CUI_KEY_K, 0x0B }, { CUI_KEY_L, 0x0C },
            { CUI_KEY_M, 0x0D }, { CUI_KEY_N, 0x0E }, { CUI_KEY_O, 0x0F },
            { CUI_KEY_P, 0x10 }, { CUI_KEY_Q, 0x11 }, { CUI_KEY_R, 0x12 },
            { CUI_KEY_S, 0x13 }, { CUI_KEY_T, 0x14 }, { CUI_KEY_U, 0x15 },
            { CUI_KEY_V, 0x16 }, { CUI_KEY_W, 0x17 }, { CUI_KEY_X, 0x18 },
            { CUI_KEY_Y, 0x19 }, { CUI_KEY_Z, 0x1A },
            { CUI_KEY_SPACE, 0x00 },   /* Ctrl+Space = NUL（nano 标记） */
        };
        if (ctrl) {
            /* Ctrl+C：有选区复制，无选区中断（原逻辑保留） */
            if (cui_key_pressed(ui, CUI_KEY_C)) {
                if (sel->has_sel) {
                    static char copybuf[64 * 1024];
                    int n = my__copy_selection(t, sel->a, sel->b, copybuf, sizeof copybuf);
                    if (n > 0 && ui->platform.set_clipboard)
                        ui->platform.set_clipboard(ui->platform.user, copybuf);
                    sel->has_sel = false;
                } else {
                    res.interrupt = true;
                }
                ui->kpressed[CUI_KEY_C] = 0;
            }

            /* Ctrl+Shift+V：粘贴（避免与 Ctrl+V=0x16 literal next 冲突） */
            if (shift && cui_key_pressed(ui, CUI_KEY_V)) {
                const char *clip = NULL;
                if (ui->platform.get_clipboard)
                    clip = ui->platform.get_clipboard(ui->platform.user);
                if (clip && clip[0]) {
                    for (const char *cp = clip; *cp && rlen < (int)sizeof raw - 1; cp++) {
                        if (*cp == '\n') raw[rlen++] = '\r';
                        else if (*cp != '\r') raw[rlen++] = *cp;
                    }
                }
                ui->kpressed[CUI_KEY_V] = 0;
            }

            /* 其余 Ctrl+字母：转控制码，并吃掉按键，防止下面 keymap 再输出字母 */
            for (int m = 0; m < (int)(sizeof ctrlmap / sizeof ctrlmap[0]); m++) {
                if (!cui_key_pressed(ui, (cui_key)ctrlmap[m].key)) continue;
                if (ctrlmap[m].key == CUI_KEY_C) continue;              /* 已单独处理 */
                if (ctrlmap[m].key == CUI_KEY_V && shift) continue;     /* 已被粘贴消费 */
                char code = ctrlmap[m].ctrl_code;
                if (rlen < (int)sizeof raw - 1) raw[rlen++] = code;
                ui->kpressed[ctrlmap[m].key] = 0;
            }

            /* Ctrl+[ = 0x1B（Esc）。注意：单独 Esc 在 ctrl 为假时不走这里 */
            if (cui_key_pressed(ui, CUI_KEY_ESCAPE) && rlen < (int)sizeof raw - 1) {
                raw[rlen++] = 0x1B;
                ui->kpressed[CUI_KEY_ESCAPE] = 0;
            }
        }

        /* ---------- 2. 可见字符（字母大小写 / 数字 / 符号），Ctrl 时不走 ---------- */
        if (!ctrl) {
            static const struct { int key; char ch; } keymap[] = {
                { CUI_KEY_A, 'a' }, { CUI_KEY_B, 'b' }, { CUI_KEY_C, 'c' },
                { CUI_KEY_D, 'd' }, { CUI_KEY_E, 'e' }, { CUI_KEY_F, 'f' },
                { CUI_KEY_G, 'g' }, { CUI_KEY_H, 'h' }, { CUI_KEY_I, 'i' },
                { CUI_KEY_J, 'j' }, { CUI_KEY_K, 'k' }, { CUI_KEY_L, 'l' },
                { CUI_KEY_M, 'm' }, { CUI_KEY_N, 'n' }, { CUI_KEY_O, 'o' },
                { CUI_KEY_P, 'p' }, { CUI_KEY_Q, 'q' }, { CUI_KEY_R, 'r' },
                { CUI_KEY_S, 's' }, { CUI_KEY_T, 't' }, { CUI_KEY_U, 'u' },
                { CUI_KEY_V, 'v' }, { CUI_KEY_W, 'w' }, { CUI_KEY_X, 'x' },
                { CUI_KEY_Y, 'y' }, { CUI_KEY_Z, 'z' },
                { CUI_KEY_SPACE, ' ' },
            };
            for (int m = 0; m < (int)(sizeof keymap / sizeof keymap[0]); m++) {
                if (!cui_key_pressed(ui, (cui_key)keymap[m].key)) continue;
                char ch = keymap[m].ch;
                if (shift && ch >= 'a' && ch <= 'z') ch -= 32;
                if (rlen < (int)sizeof raw - 1) raw[rlen++] = ch;
            }

            /* 符号键（含 Shift 形态） */
            static const struct { int key; char normal; char shifted; } symmap[] = {
                { CUI_KEY_MINUS,  '-', '_' }, { CUI_KEY_EQUAL, '=', '+' },
                { CUI_KEY_COMMA,  ',', '<' }, { CUI_KEY_PERIOD,'.', '>' },
                { CUI_KEY_SLASH,  '/', '?' }, { CUI_KEY_0, '0', ')' },
                { CUI_KEY_1, '1', '!' }, { CUI_KEY_2, '2', '@' },
                { CUI_KEY_3, '3', '#' }, { CUI_KEY_4, '4', '$' },
                { CUI_KEY_5, '5', '%' }, { CUI_KEY_6, '6', '^' },
                { CUI_KEY_7, '7', '&' }, { CUI_KEY_8, '8', '*' },
                { CUI_KEY_9, '9', '(' },
            };
            for (int m = 0; m < (int)(sizeof symmap / sizeof symmap[0]); m++) {
                if (!cui_key_pressed(ui, (cui_key)symmap[m].key)) continue;
                char ch = shift ? symmap[m].shifted : symmap[m].normal;
                if (rlen < (int)sizeof raw - 1) raw[rlen++] = ch;
            }
        }

        /* ---------- 3. 编辑键 ---------- */
        if (cui_key_pressed(ui, CUI_KEY_BACKSPACE) && rlen < (int)sizeof raw - 1)
            raw[rlen++] = 0x7f;
        if (cui_key_pressed(ui, CUI_KEY_ENTER) && rlen < (int)sizeof raw - 1)
            raw[rlen++] = '\r';
        if (cui_key_pressed(ui, CUI_KEY_TAB) && rlen < (int)sizeof raw - 1)
            raw[rlen++] = '\t';

        if (cui_key_pressed(ui, CUI_KEY_LEFT) && rlen < (int)sizeof raw - 3) {
            raw[rlen++] = 0x1b; raw[rlen++] = '['; raw[rlen++] = 'D';
        }
        if (cui_key_pressed(ui, CUI_KEY_RIGHT) && rlen < (int)sizeof raw - 3) {
            raw[rlen++] = 0x1b; raw[rlen++] = '['; raw[rlen++] = 'C';
        }
        if (cui_key_pressed(ui, CUI_KEY_UP) && rlen < (int)sizeof raw - 3) {
            raw[rlen++] = 0x1b; raw[rlen++] = '['; raw[rlen++] = 'A';
        }
        if (cui_key_pressed(ui, CUI_KEY_DOWN) && rlen < (int)sizeof raw - 3) {
            raw[rlen++] = 0x1b; raw[rlen++] = '['; raw[rlen++] = 'B';
        }
        if (cui_key_pressed(ui, CUI_KEY_HOME) && rlen < (int)sizeof raw - 3) {
            raw[rlen++] = 0x1b; raw[rlen++] = '['; raw[rlen++] = 'H';
        }
        if (cui_key_pressed(ui, CUI_KEY_END) && rlen < (int)sizeof raw - 3) {
            raw[rlen++] = 0x1b; raw[rlen++] = '['; raw[rlen++] = 'F';
        }
        if (cui_key_pressed(ui, CUI_KEY_DELETE) && rlen < (int)sizeof raw - 4) {
            raw[rlen++] = 0x1b; raw[rlen++] = '['; raw[rlen++] = '3'; raw[rlen++] = '~';
        }

        if (rlen > 0) {
            raw[rlen] = 0;
            res.raw_input = cui__frame_strdup(ui, raw, rlen);
        }
    }

    /* Enter 后强制跟随到末尾 */
    if (res.raw_input && strchr(res.raw_input, '\r')) {
        my__set_follow(vid, true);
        *top_ptr = maxtop;
    }

    cui__close(ui);
    ui->last_id = vid;
    return res;
}



