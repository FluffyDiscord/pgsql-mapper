/*
 * php_pgsql_mapper.h — PHP pgsql_mapper extension header
 */

#ifndef PHP_PGSQL_MAPPER_H
#define PHP_PGSQL_MAPPER_H

#include "php.h"
#include <libpq-fe.h>

#define PHP_PGSQL_MAPPER_VERSION "0.1.0"
#define PHP_PGSQL_MAPPER_EXTNAME "pgsql_mapper"

extern zend_module_entry pgsql_mapper_module_entry;
#define phpext_pgsql_mapper_ptr &pgsql_mapper_module_entry

/* ----------------------------------------------------------------
 * Vendored PgSql\Connection internal struct.
 *
 * PHP's ext/pgsql keeps this struct static — not exported.
 * We replicate the layout so we can extract PGconn* from the
 * zend_object* that backs a PgSql\Connection instance.
 * This MUST match the layout in ext/pgsql/pgsql_driver.c / php_pgsql.h.
 * ---------------------------------------------------------------- */
typedef struct {
    PGconn *conn;
    zend_string *hash;
    HashTable *notices;
    bool persistent;
    zend_object std;            /* must be last — standard zend_object */
} pgsql_link_handle;

static inline pgsql_link_handle *pgsql_link_from_obj(zend_object *obj) {
    return (pgsql_link_handle *)((char *)obj - XtOffsetOf(pgsql_link_handle, std));
}

/* ----------------------------------------------------------------
 * Vendored PgSql\Result internal struct.
 *
 * Same approach as above — replicate the layout to extract PGresult*.
 * This MUST match the layout in ext/pgsql/php_pgsql.h.
 * ---------------------------------------------------------------- */
typedef struct {
    PGresult *result;
    int row;
    zend_object std;                /* must be last — standard zend_object */
} pgsql_result_handle;

static inline pgsql_result_handle *pgsql_result_from_obj(zend_object *obj) {
    return (pgsql_result_handle *)((char *)obj - XtOffsetOf(pgsql_result_handle, std));
}

/* ----------------------------------------------------------------
 * PostgreSQL OID constants for the types we handle natively.
 * These are stable and defined in pg_type_d.h / server/catalog/pg_type.h.
 * ---------------------------------------------------------------- */
#define PG_OID_BOOL        16
#define PG_OID_INT8        20
#define PG_OID_INT2        21
#define PG_OID_INT4        23
#define PG_OID_FLOAT4     700
#define PG_OID_FLOAT8     701
#define PG_OID_TIMESTAMP 1114
#define PG_OID_TIMESTAMPTZ 1184

#endif /* PHP_PGSQL_MAPPER_H */
