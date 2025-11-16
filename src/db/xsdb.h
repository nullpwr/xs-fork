/*
 * xsdb.h - Embedded database engine for XS
 *
 * B-tree based key-value store with SQL-like query interface,
 * persistence, WAL journaling, and transaction support.
 */
#ifndef XSDB_H
#define XSDB_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* B-tree order (max keys per node = ORDER-1, max children = ORDER) */
#define XSDB_ORDER 64

/* Maximum columns per table */
#define XSDB_MAX_COLS 128

/* Maximum tables per database */
#define XSDB_MAX_TABLES 256

/* Maximum length of identifiers */
#define XSDB_IDENT_MAX 256

/* Maximum length of string values */
#define XSDB_VAL_MAX 4096

/* File format magic */
#define XSDB_MAGIC 0x58534442  /* "XSDB" */
#define XSDB_VERSION 1

/* WAL record types */
#define XSDB_WAL_INSERT  1
#define XSDB_WAL_DELETE  2
#define XSDB_WAL_UPDATE  3
#define XSDB_WAL_CREATE  4
#define XSDB_WAL_DROP    5
#define XSDB_WAL_COMMIT  6
#define XSDB_WAL_BEGIN   7

/* Column types */
typedef enum {
    XSDB_TYPE_TEXT = 0,
    XSDB_TYPE_INT,
    XSDB_TYPE_FLOAT,
    XSDB_TYPE_BLOB,
    XSDB_TYPE_NULL,
    XSDB_TYPE_AUTO   /* auto-detect from value */
} XSDBType;

/* Comparison operators for WHERE clauses */
typedef enum {
    XSDB_CMP_EQ = 0,   /* = */
    XSDB_CMP_NE,       /* != or <> */
    XSDB_CMP_LT,       /* < */
    XSDB_CMP_LE,       /* <= */
    XSDB_CMP_GT,       /* > */
    XSDB_CMP_GE,       /* >= */
    XSDB_CMP_LIKE,     /* LIKE */
    XSDB_CMP_IS_NULL,  /* IS NULL */
    XSDB_CMP_NOT_NULL  /* IS NOT NULL */
} XSDBCmpOp;

/* Logical operators for combining WHERE conditions */
typedef enum {
    XSDB_LOGIC_NONE = 0,
    XSDB_LOGIC_AND,
    XSDB_LOGIC_OR
} XSDBLogicOp;

/* ORDER BY direction */
typedef enum {
    XSDB_ORDER_ASC = 0,
    XSDB_ORDER_DESC
} XSDBOrderDir;

/* SQL statement types */
typedef enum {
    XSDB_SQL_CREATE_TABLE = 0,
    XSDB_SQL_DROP_TABLE,
    XSDB_SQL_INSERT,
    XSDB_SQL_SELECT,
    XSDB_SQL_UPDATE,
    XSDB_SQL_DELETE,
    XSDB_SQL_BEGIN,
    XSDB_SQL_COMMIT,
    XSDB_SQL_ROLLBACK,
    XSDB_SQL_CREATE_INDEX,
    XSDB_SQL_ALTER_TABLE
} XSDBStmtType;

/* A single cell value */
typedef struct {
    XSDBType type;
    union {
        char    *text;
        int64_t  ival;
        double   fval;
        struct {
            uint8_t *data;
            size_t   len;
        } blob;
    };
} XSDBValue;

/* A row is an array of values plus a rowid */
typedef struct {
    int64_t    rowid;
    XSDBValue *cells;
    int        ncells;
} XSDBRow;

/* B-tree node */
typedef struct XSDBBTreeNode {
    int64_t  keys[XSDB_ORDER - 1];
    XSDBRow *rows[XSDB_ORDER - 1];
    struct XSDBBTreeNode *children[XSDB_ORDER];
    int      nkeys;
    int      is_leaf;
} XSDBBTreeNode;

/* Column definition */
typedef struct {
    char      name[XSDB_IDENT_MAX];
    XSDBType  type;
    int       not_null;
    int       primary_key;
    int       unique;
    int       has_default;
    XSDBValue default_val;
} XSDBColumn;

/* Index definition */
typedef struct XSDBIndex {
    char     name[XSDB_IDENT_MAX];
    char     col_name[XSDB_IDENT_MAX];
    int      col_idx;
    int      unique;
    XSDBBTreeNode *root;
    struct XSDBIndex *next;
} XSDBIndex;

/* Table definition */
typedef struct {
    char          name[XSDB_IDENT_MAX];
    XSDBColumn   *columns;
    int           ncols;
    XSDBBTreeNode *btree;      /* primary B-tree indexed by rowid */
    int64_t       next_rowid;
    int64_t       row_count;
    XSDBIndex    *indexes;
} XSDBTable;

/* WHERE condition */
typedef struct XSDBWhere {
    char          col[XSDB_IDENT_MAX];
    XSDBCmpOp     op;
    XSDBValue     val;
    XSDBLogicOp   logic;       /* how this condition connects to the next */
    struct XSDBWhere *next;
} XSDBWhere;

/* SET clause for UPDATE */
typedef struct XSDBSet {
    char        col[XSDB_IDENT_MAX];
    XSDBValue   val;
    struct XSDBSet *next;
} XSDBSet;

/* ORDER BY clause */
typedef struct {
    char         col[XSDB_IDENT_MAX];
    XSDBOrderDir dir;
} XSDBOrderBy;

/* LIMIT clause */
typedef struct {
    int has_limit;
    int64_t limit;
    int has_offset;
    int64_t offset;
} XSDBLimit;

/* Parsed SQL statement */
typedef struct {
    XSDBStmtType type;
    char         table[XSDB_IDENT_MAX];
    /* For CREATE TABLE */
    XSDBColumn  *create_cols;
    int          ncreate_cols;
    /* For INSERT */
    XSDBValue  **insert_rows;   /* array of value arrays */
    int          ninsert_rows;
    int          ninsert_cols;
    char       **insert_col_names;  /* explicit column list, or NULL */
    int          ninsert_col_names;
    /* For SELECT */
    char       **select_cols;
    int          nselect_cols;
    int          select_all;    /* SELECT * */
    int          select_count;  /* SELECT COUNT(*) */
    /* For UPDATE */
    XSDBSet     *set_list;
    /* WHERE, ORDER BY, LIMIT (shared) */
    XSDBWhere   *where;
    XSDBOrderBy *order_by;
    int          norder_by;
    XSDBLimit    limit;
    /* For CREATE INDEX */
    char         index_name[XSDB_IDENT_MAX];
    char         index_col[XSDB_IDENT_MAX];
    int          index_unique;
    /* For ALTER TABLE */
    char         alter_col[XSDB_IDENT_MAX];
    XSDBType     alter_type;
    int          alter_add;     /* 1 = ADD COLUMN, 0 = DROP COLUMN */
} XSDBStmt;

/* Prepared statement */
typedef struct {
    XSDBStmt  *parsed;
    char      *sql;
    int        nparam_slots;
    XSDBValue *bound_params;
    int        nbound;
} XSDBPrepared;

/* WAL record */
typedef struct {
    uint8_t  type;
    uint32_t table_len;
    uint32_t data_len;
    /* followed by: table_name, then data */
} XSDBWalRecord;

/* Transaction snapshot for rollback */
typedef struct XSDBSnapshot {
    XSDBTable         *tables;
    int                ntables;
    struct XSDBSnapshot *prev;  /* for nested transactions */
} XSDBSnapshot;

/* Result set */
typedef struct {
    XSDBRow   *rows;
    int        nrows;
    int        cap;
    char     **col_names;
    int        ncols;
    int        affected;    /* rows affected (for INSERT/UPDATE/DELETE) */
    char      *error;       /* error message, or NULL */
} XSDBResult;

/* The database handle */
typedef struct {
    char          *path;         /* file path, or NULL for in-memory */
    XSDBTable     *tables;
    int            ntables;
    int            table_cap;
    int            in_transaction;
    FILE          *wal_fp;       /* WAL file handle */
    char          *wal_path;
    XSDBSnapshot  *snapshot;     /* for rollback */
    int            auto_commit;
    int            closed;
} XSDB;

/* Database lifecycle */
XSDB       *xsdb_open(const char *path);  /* NULL path = in-memory */
void        xsdb_close(XSDB *db);

/* SQL execution */
XSDBResult *xsdb_exec(XSDB *db, const char *sql);
XSDBResult *xsdb_exec_prepared(XSDB *db, XSDBPrepared *stmt);

/* Prepared statements */
XSDBPrepared *xsdb_prepare(XSDB *db, const char *sql);
int           xsdb_bind_int(XSDBPrepared *stmt, int idx, int64_t val);
int           xsdb_bind_float(XSDBPrepared *stmt, int idx, double val);
int           xsdb_bind_text(XSDBPrepared *stmt, int idx, const char *val);
int           xsdb_bind_null(XSDBPrepared *stmt, int idx);
void          xsdb_prepared_free(XSDBPrepared *stmt);

/* Transaction control */
int         xsdb_begin(XSDB *db);
int         xsdb_commit(XSDB *db);
int         xsdb_rollback(XSDB *db);

/* Table introspection */
int         xsdb_table_count(XSDB *db);
const char *xsdb_table_name(XSDB *db, int idx);
int         xsdb_table_exists(XSDB *db, const char *name);

/* Persistence */
int         xsdb_save(XSDB *db, const char *path);
XSDB       *xsdb_load(const char *path);

/* Result helpers */
void        xsdb_result_free(XSDBResult *r);
XSDBValue  *xsdb_result_get(XSDBResult *r, int row, int col);
const char *xsdb_result_col_name(XSDBResult *r, int col);

/* Value helpers */
XSDBValue   xsdb_value_text(const char *s);
XSDBValue   xsdb_value_int(int64_t i);
XSDBValue   xsdb_value_float(double f);
XSDBValue   xsdb_value_null(void);
void        xsdb_value_free(XSDBValue *v);
char       *xsdb_value_to_string(const XSDBValue *v);
int         xsdb_value_compare(const XSDBValue *a, const XSDBValue *b);

/* B-tree operations (exposed for testing) */
XSDBBTreeNode *xsdb_btree_new_node(int is_leaf);
int            xsdb_btree_insert(XSDBBTreeNode **root, int64_t key, XSDBRow *row);
XSDBRow       *xsdb_btree_search(XSDBBTreeNode *root, int64_t key);
int            xsdb_btree_delete(XSDBBTreeNode **root, int64_t key);
void           xsdb_btree_scan(XSDBBTreeNode *root,
                                void (*cb)(XSDBRow *row, void *ctx), void *ctx);
void           xsdb_btree_free(XSDBBTreeNode *root);
int            xsdb_btree_count(XSDBBTreeNode *root);

/* SQL parser (exposed for prepared statements) */
XSDBStmt *xsdb_parse_sql(const char *sql);
void      xsdb_stmt_free(XSDBStmt *stmt);

#endif /* XSDB_H */
