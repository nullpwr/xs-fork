#ifndef XS_FMT_H
#define XS_FMT_H

#include "core/ast.h"
#include "core/lexer.h"

/* Format configuration */
typedef struct {
    int indent_width;   /* spaces per indent level (default 4) */
    int max_line_width; /* wrap threshold (default 100, 0 = no wrap) */
    int trailing_commas;/* add trailing commas in multi-line collections */
    int start_line;     /* format from this line (1-based, 0 = start) */
    int end_line;       /* format to this line (0 = end of file) */
} FmtConfig;

/* Initialize config with defaults */
void  fmt_config_default(FmtConfig *cfg);

/* Try to read [format] section from xs.toml in same dir as path */
void  fmt_config_from_toml(FmtConfig *cfg, const char *path);

/* Format AST to canonical source. Caller frees. Comments from src are preserved. */
char *fmt_format(Node *program, const char *src);
char *fmt_format_ex(Node *program, const char *src, const FmtConfig *cfg);
int   fmt_file(const char *path);
int   fmt_file_check(const char *path);

/* Format only a range of lines (for LSP integration).
   Returns formatted text for the range. Caller frees. */
char *fmt_range(const char *path, int start_line, int end_line);

#endif
