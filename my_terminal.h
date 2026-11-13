#ifndef MY_TERMINAL_H
#define MY_TERMINAL_H

#include <cui/cui.h>

/* 直接复用 cui.h 的 cui_terminal_line / cui_terminal_buffer，
 * 只在 buffer 上多一个 cursor 字段——不行，cui_terminal_buffer 是 cui.h 定义的，
 * 我们不能改它。所以用 cursor 存储在别处（见下面）。 */

typedef struct my_terminal_opts {
    int _;
    cui_size width;
    float height;
    float size;
    const char *prompt;
    bool no_input;
    char lang;
} my_terminal_opts;

typedef struct my_terminal_result {
    const char *raw_input;
    bool interrupt;
    bool select_all;      /* 全选 */
    bool copy;            /* 复制选中 */
    bool paste;           /* 粘贴剪贴板 */
    bool clear; 
} my_terminal_result;

void my_terminal_init(cui_terminal_buffer *t, char *text, size_t text_cap,
                      cui_terminal_line *lines, int line_cap);
void my_terminal_write(cui_terminal_buffer *t, const char *data, int len);
void my_terminal_printf(cui_terminal_buffer *t, const char *fmt, ...);
void my_terminal_clear(cui_terminal_buffer *t);

my_terminal_result my_terminal_ex(cui_ctx *ui, const char *id_str,
                                  cui_terminal_buffer *t,
                                  const my_terminal_opts *opts);

#define my_terminal(ui, id, t, ...) \
    my_terminal_ex((ui), (id), (t), CUI__OPTS(my_terminal_opts, __VA_ARGS__))

#endif