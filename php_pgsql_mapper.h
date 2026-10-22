#ifndef PHP_PGSQL_MAPPER_H
#define PHP_PGSQL_MAPPER_H

#include "php.h"
#include <libpq-fe.h>

#define PHP_PGSQL_MAPPER_VERSION "1.2.1"
#define PHP_PGSQL_MAPPER_EXTNAME "pgsql_mapper"

extern zend_module_entry pgsql_mapper_module_entry;
#define phpext_pgsql_mapper_ptr &pgsql_mapper_module_entry

typedef struct {
    PGconn *conn;
    PGresult *result;
    int row;
    zend_object std;
} pgsql_result_handle;

static inline pgsql_result_handle *pgsql_result_from_obj(zend_object *object)
{
    return (pgsql_result_handle *)((char *)object - XtOffsetOf(pgsql_result_handle, std));
}

#define PG_OID_BOOL 16
#define PG_OID_INT8 20
#define PG_OID_INT2 21
#define PG_OID_INT4 23
#define PG_OID_TEXT 25
#define PG_OID_JSON 114
#define PG_OID_FLOAT4 700
#define PG_OID_FLOAT8 701
#define PG_OID_DATE 1082
#define PG_OID_TIMESTAMP 1114
#define PG_OID_TIMESTAMPTZ 1184
#define PG_OID_JSONB 3802

#define PG_OID_BOOL_ARRAY 1000
#define PG_OID_INT2_ARRAY 1005
#define PG_OID_INT4_ARRAY 1007
#define PG_OID_INT8_ARRAY 1016
#define PG_OID_BOX_ARRAY 1020
#define PG_OID_FLOAT4_ARRAY 1021
#define PG_OID_FLOAT8_ARRAY 1022
#define PG_OID_TIMESTAMP_ARRAY 1115
#define PG_OID_TIMESTAMPTZ_ARRAY 1185

#endif
