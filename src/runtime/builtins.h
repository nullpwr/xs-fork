#ifndef BUILTINS_H
#define BUILTINS_H

#include "core/value.h"

struct Interp;

void stdlib_register(struct Interp *i);

/* Lazy stdlib lookup. Returns a fresh module Value (caller owns the
   ref), or NULL if the name isn't a stdlib module. Called by
   NODE_IMPORT and OP_IMPORT after env / globals lookup misses. */
Value *stdlib_load_module(struct Interp *i, const char *name);

Value *call_value(struct Interp *i, Value *callee, Value **args, int argc,
                  const char *call_site);

// module factories (tree-walker + VM)
Value *make_math_module(void);
Value *make_time_module(void);
Value *make_string_module(void);
Value *make_path_module(void);
Value *make_base64_module(void);
Value *make_hash_module(void);
Value *make_uuid_module(void);
Value *make_collections_module(void);
Value *make_random_module(void);
Value *make_json_module(void);
Value *make_log_module(void);
Value *make_fmt_module(void);
Value *make_csv_module(void);
Value *make_url_module(void);
Value *make_re_module(void);
Value *make_process_module(void);
Value *make_io_module(void);
Value *make_os_module(struct Interp *ig);

/* shared stdlib helpers exposed across the split builtins files */
int   xs_io_mkdirs(const char *path);
extern int    g_xs_argc;
extern char **g_xs_argv;
Value *make_async_module(void);
Value *make_net_module(void);
Value *make_crypto_module(void);
Value *make_thread_module(void);
Value *make_buf_module(void);
Value *make_encode_module(void);
Value *make_db_module(void);
Value *make_cli_module(void);
Value *make_ffi_module(void);
Value *make_reflect_module(void);
Value *make_gc_module(void);
Value *make_toml_module(void);
Value *make_http_module(void);
Value *make_fs_module(void);
Value *make_test_module(void);
Value *make_tracing_module(void);

#endif
