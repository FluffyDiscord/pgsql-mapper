/*
 * pgsql_mapper.c — pg_fast_query() implementation
 *
 * Hydrates PostgreSQL rows directly into typed PHP objects from C,
 * bypassing intermediate userland arrays. Supports nested object mapping
 * via dot-delimited column names (e.g. SELECT account_id AS "address.id").
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/date/php_date.h"
#include "zend_exceptions.h"

#include "php_pgsql_mapper.h"

#define MAX_NESTING_DEPTH 8

/* ----------------------------------------------------------------
 * Column-to-property mapping with nested object support.
 *
 * depth == 1: flat property on root object
 * depth > 1:  dot-separated path (or auto-expanded class-typed flat prop)
 *
 * ces[i] is the CE of the object that OWNS the property at level i.
 * ces[0] = root CE. ces[i] for i>0 is resolved from props[i-1]'s type.
 * ---------------------------------------------------------------- */
typedef struct {
    int depth;
    zend_string *names[MAX_NESTING_DEPTH];
    zend_class_entry *ces[MAX_NESTING_DEPTH];
    zend_property_info *props[MAX_NESTING_DEPTH];
    uint32_t offsets[MAX_NESTING_DEPTH];
    bool skip_type_check;
    bool accepts_null;
    bool valid;
} col_mapping;

/* Per-property mapping for constructor dot-column nesting */
typedef struct {
    int col_idx;
    uint32_t prop_offset;
    bool skip_type_check;
    bool accepts_null;
} ctor_nest_prop;

/* Info for building a nested object from dot-columns for a ctor param */
typedef struct {
    zend_class_entry *ce;
    uint32_t count;
    ctor_nest_prop *props;
} ctor_nest_info;

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

/* Resolve class entry from a property's declared type.
 * Returns NULL for untyped, union/intersection, or non-class types. */
static zend_class_entry *resolve_property_class(zend_property_info *prop)
{
    if (!prop || !ZEND_TYPE_IS_SET(prop->type)) return NULL;
    if (!ZEND_TYPE_HAS_NAME(prop->type)) return NULL;
    return zend_lookup_class(ZEND_TYPE_NAME(prop->type));
}

/* Get the first declared public non-static property of a class. */
static bool get_first_property(zend_class_entry *ce, zend_string **name_out,
                               zend_property_info **prop_out)
{
    zend_string *pname;
    zend_property_info *pinfo;
    ZEND_HASH_FOREACH_STR_KEY_PTR(&ce->properties_info, pname, pinfo) {
        if (pinfo->flags & ZEND_ACC_STATIC) continue;
        if (!(pinfo->flags & ZEND_ACC_PUBLIC)) continue;
#if PHP_VERSION_ID >= 80400
        if (pinfo->hooks) continue;
#endif
        *name_out = pname;
        *prop_out = pinfo;
        return true;
    } ZEND_HASH_FOREACH_END();
    return false;
}

/* Resolve a public, non-static, non-hooked property on a class. */
static zend_property_info *resolve_public_property(zend_class_entry *ce, zend_string *name)
{
    zend_property_info *prop = zend_hash_find_ptr(&ce->properties_info, name);
    if (!prop) return NULL;
    if (prop->flags & ZEND_ACC_STATIC) return NULL;
    if (!(prop->flags & ZEND_ACC_PUBLIC)) return NULL;
#if PHP_VERSION_ID >= 80400
    if (prop->hooks) return NULL;
#endif
    return prop;
}

/* Check if the zval type produced by this OID is compatible with the declared property type.
 * Only covers non-null values; null acceptability is checked separately. */
static bool can_skip_type_check(const zend_property_info *info, Oid oid, zend_class_entry *datetime_ce)
{
    if (!ZEND_TYPE_IS_SET(info->type)) {
        return true;
    }

    uint32_t mask = ZEND_TYPE_FULL_MASK(info->type);

    switch (oid) {
        case PG_OID_INT2:
        case PG_OID_INT4:
        case PG_OID_INT8:
            return (mask & MAY_BE_LONG) != 0;

        case PG_OID_FLOAT4:
        case PG_OID_FLOAT8:
            return (mask & MAY_BE_DOUBLE) != 0;

        case PG_OID_BOOL:
            return (mask & (MAY_BE_TRUE | MAY_BE_FALSE)) == (MAY_BE_TRUE | MAY_BE_FALSE);

        case PG_OID_TIMESTAMP:
        case PG_OID_TIMESTAMPTZ:
            if (ZEND_TYPE_HAS_NAME(info->type)) {
                zend_class_entry *type_ce = zend_lookup_class(ZEND_TYPE_NAME(info->type));
                if (type_ce && instanceof_function(datetime_ce, type_ce)) {
                    return true;
                }
            }
            return false;

        default:
            return (mask & MAY_BE_STRING) != 0;
    }
}

/* Check if a CE is a fast-path-compatible plain class. */
static bool is_fast_path_class(zend_class_entry *ce)
{
    return ce->__set == NULL && ce->create_object == NULL;
}

/* Check if a constructor only assigns promoted properties (no function calls,
 * no custom body logic). If so, we can skip the VM execution entirely and
 * write to property slots directly via OBJ_PROP. */
static bool is_trivial_promoted_ctor(zend_function *ctor, zend_class_entry *ce)
{
    if (ctor->type != ZEND_USER_FUNCTION) return false;
    if (ctor->common.fn_flags & ZEND_ACC_VARIADIC) return false;

    uint32_t num_args = ctor->common.num_args;
    if (num_args == 0) return false;

    /* All params must have matching public non-static properties (promoted) */
    for (uint32_t i = 0; i < num_args; i++) {
        zend_string *pname = ctor->common.arg_info[i].name;
        zend_property_info *prop = zend_hash_find_ptr(&ce->properties_info, pname);
        if (!prop) return false;
        if (prop->flags & ZEND_ACC_STATIC) return false;
        if (!(prop->flags & ZEND_ACC_PUBLIC)) return false;
    }

    /* Scan opcodes: only allow the pattern generated by pure promoted-property ctors.
     * Any function call, throw, yield, new, or complex logic → non-trivial. */
    zend_op *opcodes = ctor->op_array.opcodes;
    uint32_t last = ctor->op_array.last;
    for (uint32_t i = 0; i < last; i++) {
        switch (opcodes[i].opcode) {
            case ZEND_RECV:
            case ZEND_RECV_INIT:
            case ZEND_ASSIGN_OBJ:
            case ZEND_RETURN:
            case ZEND_NOP:
            case ZEND_EXT_STMT:
            case ZEND_EXT_NOP:
                continue;
            default:
                return false;
        }
    }

    return true;
}

/* Fast decimal integer parser — PG guarantees valid decimal digits.
 * Avoids strtoll overhead (base detection, errno, whitespace handling). */
static zend_always_inline zend_long fast_parse_long(const char *s)
{
    zend_long val = 0;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return neg ? -val : val;
}

/* DateTime conversion — kept out-of-line (cold path). */
static void convert_pg_datetime(zval *val, const char *raw, int len,
                                zend_class_entry *datetime_ce)
{
    zval dt_obj;
    php_date_instantiate(datetime_ce, &dt_obj);
    php_date_obj *dt = Z_PHPDATE_P(&dt_obj);
    if (!php_date_initialize(dt, raw, len, NULL, NULL, 0)) {
        zval_ptr_dtor(&dt_obj);
        ZVAL_NULL(val);
    } else {
        ZVAL_COPY_VALUE(val, &dt_obj);
    }
}

/* Convert a non-NULL PG value to a zval based on column OID.
 * Fast cases are force-inlined; datetime stays out-of-line. */
static zend_always_inline void convert_pg_value(zval *val, const char *raw, int len,
                                                 Oid oid, zend_class_entry *datetime_ce)
{
    switch (oid) {
        case PG_OID_INT2:
        case PG_OID_INT4:
        case PG_OID_INT8:
            ZVAL_LONG(val, fast_parse_long(raw));
            break;

        case PG_OID_FLOAT4:
        case PG_OID_FLOAT8:
            ZVAL_DOUBLE(val, zend_strtod(raw, NULL));
            break;

        case PG_OID_BOOL:
            ZVAL_BOOL(val, raw[0] == 't' || raw[0] == 'T');
            break;

        case PG_OID_TIMESTAMP:
        case PG_OID_TIMESTAMPTZ:
            convert_pg_datetime(val, raw, len, datetime_ce);
            break;

        default:
            ZVAL_STRINGL(val, raw, len);
            break;
    }
}

/* ----------------------------------------------------------------
 * Column mapping builder
 *
 * Splits column name by '.' to build a property path.
 * For flat columns (no dot) whose property type is a non-DateTime class,
 * auto-expands to depth 2: creates nested object, sets first property.
 * ---------------------------------------------------------------- */
static void build_col_mapping(col_mapping *m, const char *col_name, size_t col_len,
                              zend_class_entry *root_ce, Oid oid, zend_class_entry *datetime_ce)
{
    memset(m, 0, sizeof(*m));

    /* Split by '.' */
    const char *segments[MAX_NESTING_DEPTH];
    size_t seg_lens[MAX_NESTING_DEPTH];
    int nseg = 0;

    const char *p = col_name;
    const char *end = col_name + col_len;
    while (p < end && nseg < MAX_NESTING_DEPTH) {
        const char *dot = memchr(p, '.', end - p);
        if (!dot) dot = end;
        if (dot == p) return; /* empty segment */
        segments[nseg] = p;
        seg_lens[nseg] = dot - p;
        nseg++;
        p = dot + 1;
    }

    if (nseg == 0 || nseg > MAX_NESTING_DEPTH) return;

    /* Resolve property chain */
    zend_class_entry *current_ce = root_ce;

    for (int i = 0; i < nseg; i++) {
        m->names[i] = zend_string_init(segments[i], seg_lens[i], 0);
        m->ces[i] = current_ce;

        zend_property_info *prop = resolve_public_property(current_ce, m->names[i]);
        if (!prop) return;

        m->props[i] = prop;
        m->offsets[i] = prop->offset;

        if (i < nseg - 1) {
            /* Intermediate segment: resolve nested CE from property type */
            zend_class_entry *child_ce = resolve_property_class(prop);
            if (!child_ce || !is_fast_path_class(child_ce)) return;
            current_ce = child_ce;
        }
    }

    m->depth = nseg;

    /* Auto-nest: flat column (no dot) where property is a non-DateTime class.
     * Expand to depth 2: value goes to the first property of the nested class. */
    if (nseg == 1) {
        zend_class_entry *prop_ce = resolve_property_class(m->props[0]);
        if (prop_ce
            && !instanceof_function(prop_ce, php_date_get_interface_ce())
            && is_fast_path_class(prop_ce)) {
            zend_string *first_name = NULL;
            zend_property_info *first_prop = NULL;
            if (!get_first_property(prop_ce, &first_name, &first_prop)) return;

            m->depth = 2;
            m->names[1] = zend_string_copy(first_name);
            m->ces[1] = prop_ce;
            m->props[1] = first_prop;
            m->offsets[1] = first_prop->offset;
        }
    }

    /* Leaf type check info */
    int leaf = m->depth - 1;
    m->skip_type_check = can_skip_type_check(m->props[leaf], oid, datetime_ce);
    m->accepts_null = !ZEND_TYPE_IS_SET(m->props[leaf]->type) ||
                      (ZEND_TYPE_FULL_MASK(m->props[leaf]->type) & MAY_BE_NULL);
    m->valid = true;
}

static void free_col_mapping(col_mapping *m)
{
    for (int i = 0; i < MAX_NESTING_DEPTH; i++) {
        if (m->names[i]) zend_string_release(m->names[i]);
    }
}

/* ----------------------------------------------------------------
 * Cached PgSql\Result class entry — resolved lazily at first call
 * ---------------------------------------------------------------- */
static zend_class_entry *pgsql_result_ce = NULL;

static zend_class_entry *resolve_pgsql_result_ce(void)
{
    if (pgsql_result_ce) {
        return pgsql_result_ce;
    }
    zend_string *cn = zend_string_init("PgSql\\Result", sizeof("PgSql\\Result") - 1, 0);
    pgsql_result_ce = zend_lookup_class(cn);
    zend_string_release(cn);
    return pgsql_result_ce;
}

/* {{{ pg_fast_query(PgSql\Result $result, string $class): array */
PHP_FUNCTION(pg_fast_query)
{
    zval *zresult;
    zend_string *class_name;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT(zresult)
        Z_PARAM_STR(class_name)
    ZEND_PARSE_PARAMETERS_END();

    /* Resolve PgSql\Result CE (lazy, cached) */
    zend_class_entry *result_ce = resolve_pgsql_result_ce();
    if (!result_ce) {
        zend_throw_exception(zend_ce_error,
            "pg_fast_query(): PgSql\\Result class not found — is ext/pgsql loaded?", 0);
        RETURN_THROWS();
    }

    if (!instanceof_function(Z_OBJCE_P(zresult), result_ce)) {
        zend_throw_exception(zend_ce_type_error,
            "pg_fast_query(): argument #1 must be of type PgSql\\Result", 0);
        RETURN_THROWS();
    }

    /* Look up target class */
    zend_class_entry *ce = zend_lookup_class(class_name);
    if (!ce) {
        zend_throw_exception_ex(zend_ce_value_error, 0,
            "pg_fast_query(): class \"%s\" not found", ZSTR_VAL(class_name));
        RETURN_THROWS();
    }

    /* Extract PGresult* from PgSql\Result object (not owned by us — do NOT PQclear) */
    pgsql_result_handle *rh = pgsql_result_from_obj(Z_OBJ_P(zresult));
    PGresult *res = rh->result;

    if (!res) {
        zend_throw_exception(zend_ce_error,
            "pg_fast_query(): result handle is invalid", 0);
        RETURN_THROWS();
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_TUPLES_OK) {
        zend_throw_exception_ex(zend_ce_exception, 0,
            "pg_fast_query(): result does not contain rows (status: %s)",
            PQresStatus(status));
        RETURN_THROWS();
    }

    int nrows = PQntuples(res);
    int ncols = PQnfields(res);

    /* Pre-fetch column OIDs */
    Oid *col_oids = safe_emalloc(ncols, sizeof(Oid), 0);
    for (int c = 0; c < ncols; c++) {
        col_oids[c] = PQftype(res, c);
    }

    /* DateTimeImmutable class entry */
    zend_class_entry *datetime_ce = php_date_get_immutable_ce();

    /* Build column mappings (fast path) or intern column names (slow path) */
    bool class_fast_path = is_fast_path_class(ce);
    col_mapping *mappings = NULL;
    zend_string **col_names = NULL;

    if (class_fast_path) {
        mappings = safe_emalloc(ncols, sizeof(col_mapping), 0);
        for (int c = 0; c < ncols; c++) {
            const char *fname = PQfname(res, c);
            build_col_mapping(&mappings[c], fname, strlen(fname), ce, col_oids[c], datetime_ce);
        }
    } else {
        col_names = safe_emalloc(ncols, sizeof(zend_string *), 0);
        for (int c = 0; c < ncols; c++) {
            const char *fname = PQfname(res, c);
            col_names[c] = zend_new_interned_string(
                zend_string_init(fname, strlen(fname), 0)
            );
        }
    }

    /* ---- Constructor analysis ---- */
    bool has_ctor = (ce->constructor != NULL);
    uint32_t ctor_num_params = has_ctor ? ce->constructor->common.num_args : 0;
    uint32_t ctor_all_params = ctor_num_params; /* original count for cleanup */
    int *ctor_param_col = NULL;              /* [param] -> col index, or -1 */
    bool *col_is_ctor_arg = NULL;            /* [col]   -> true if consumed by ctor */
    zend_class_entry **ctor_param_nest_ce = NULL;   /* [param] -> CE for auto-nest or NULL */
    uint32_t *ctor_param_nest_offset = NULL;        /* [param] -> first-prop offset */
    ctor_nest_info **ctor_param_dot = NULL;         /* [param] -> dot-col nesting or NULL */
    zval *ctor_args = NULL;                         /* positional args array */
    uint32_t ctor_positional_count = 0;             /* number of positional args to pass */
    bool ctor_use_positional = false;               /* true = no gaps, use positional */
    bool ctor_all_cols = false;                     /* true = all columns consumed by ctor */
    bool ctor_ht_inited = false;
    HashTable ctor_named_ht;

    /* Trivial promoted-property-only ctor: bypass VM entirely, use fast-path writes */
    if (has_ctor && class_fast_path &&
        is_trivial_promoted_ctor(ce->constructor, ce)) {
        has_ctor = false;
        ctor_num_params = 0;
    }

    if (has_ctor && ctor_num_params > 0) {
        zend_function *ctor = ce->constructor;
        zend_arg_info *arg_info = ctor->common.arg_info;

        ctor_param_col = safe_emalloc(ctor_num_params, sizeof(int), 0);
        col_is_ctor_arg = ecalloc(ncols, sizeof(bool));
        ctor_param_nest_ce = ecalloc(ctor_num_params, sizeof(zend_class_entry *));
        ctor_param_nest_offset = safe_emalloc(ctor_num_params, sizeof(uint32_t), 0);
        ctor_param_dot = ecalloc(ctor_num_params, sizeof(ctor_nest_info *));

        /* Pass 1: match params to flat columns by name */
        for (uint32_t p = 0; p < ctor_num_params; p++) {
            ctor_param_col[p] = -1;
            ctor_param_nest_offset[p] = 0;
            zend_string *pname = arg_info[p].name;

            for (int c = 0; c < ncols; c++) {
                if (col_is_ctor_arg[c]) continue;
                const char *col = PQfname(res, c);
                size_t col_len = strlen(col);
                if (ZSTR_LEN(pname) == col_len &&
                    memcmp(ZSTR_VAL(pname), col, col_len) == 0) {
                    ctor_param_col[p] = c;
                    col_is_ctor_arg[c] = true;
                    break;
                }
            }

            /* Auto-nest: flat column matched to a non-DateTime user class param */
            if (ctor_param_col[p] >= 0 &&
                ZEND_TYPE_IS_SET(arg_info[p].type) &&
                ZEND_TYPE_HAS_NAME(arg_info[p].type)) {
                zend_class_entry *pce = zend_lookup_class(ZEND_TYPE_NAME(arg_info[p].type));
                if (pce &&
                    !instanceof_function(pce, php_date_get_interface_ce()) &&
                    is_fast_path_class(pce)) {
                    zend_string *fn;
                    zend_property_info *fp;
                    if (get_first_property(pce, &fn, &fp)) {
                        ctor_param_nest_ce[p] = pce;
                        ctor_param_nest_offset[p] = fp->offset;
                    }
                }
            }
        }

        /* Pass 2: for unmatched class-typed params, collect dot-columns (e.g. "nested.yep") */
        for (uint32_t p = 0; p < ctor_num_params; p++) {
            if (ctor_param_col[p] >= 0) continue;
            if (!ZEND_TYPE_IS_SET(arg_info[p].type) || !ZEND_TYPE_HAS_NAME(arg_info[p].type)) continue;

            zend_class_entry *pce = zend_lookup_class(ZEND_TYPE_NAME(arg_info[p].type));
            if (!pce || instanceof_function(pce, php_date_get_interface_ce()) || !is_fast_path_class(pce)) continue;

            zend_string *pname = arg_info[p].name;
            size_t pname_len = ZSTR_LEN(pname);
            const char *pname_val = ZSTR_VAL(pname);

            /* Count matching dot-columns */
            uint32_t dot_count = 0;
            for (int c = 0; c < ncols; c++) {
                if (col_is_ctor_arg[c]) continue;
                const char *col = PQfname(res, c);
                size_t col_len = strlen(col);
                if (col_len > pname_len + 1 && col[pname_len] == '.' &&
                    memcmp(col, pname_val, pname_len) == 0) {
                    dot_count++;
                }
            }
            if (dot_count == 0) continue;

            /* Build property mappings */
            ctor_nest_prop *dot_props = safe_emalloc(dot_count, sizeof(ctor_nest_prop), 0);
            uint32_t idx = 0;

            for (int c = 0; c < ncols; c++) {
                if (col_is_ctor_arg[c]) continue;
                const char *col = PQfname(res, c);
                size_t col_len = strlen(col);
                if (col_len > pname_len + 1 && col[pname_len] == '.' &&
                    memcmp(col, pname_val, pname_len) == 0) {

                    const char *sub = col + pname_len + 1;
                    size_t sub_len = col_len - pname_len - 1;
                    zend_string *sn = zend_string_init(sub, sub_len, 0);
                    zend_property_info *pi = resolve_public_property(pce, sn);
                    zend_string_release(sn);

                    if (pi) {
                        dot_props[idx].col_idx = c;
                        dot_props[idx].prop_offset = pi->offset;
                        dot_props[idx].skip_type_check = can_skip_type_check(pi, col_oids[c], datetime_ce);
                        dot_props[idx].accepts_null = !ZEND_TYPE_IS_SET(pi->type) ||
                            (ZEND_TYPE_FULL_MASK(pi->type) & MAY_BE_NULL);
                        col_is_ctor_arg[c] = true;
                        idx++;
                    }
                }
            }

            if (idx > 0) {
                ctor_param_dot[p] = emalloc(sizeof(ctor_nest_info));
                ctor_param_dot[p]->ce = pce;
                ctor_param_dot[p]->count = idx;
                ctor_param_dot[p]->props = dot_props;
            } else {
                efree(dot_props);
            }
        }

        /* Determine if we can use positional args (no gaps in matched params) */
        ctor_use_positional = true;
        for (uint32_t p = 0; p < ctor_num_params; p++) {
            if (ctor_param_col[p] < 0 && !ctor_param_dot[p]) {
                /* Unmatched param — check if all subsequent are also unmatched (trailing optionals OK) */
                for (uint32_t q = p; q < ctor_num_params; q++) {
                    if (ctor_param_col[q] >= 0 || ctor_param_dot[q]) {
                        /* Gap: matched param after unmatched — can't use positional */
                        ctor_use_positional = false;
                        break;
                    }
                }
                break; /* Either we set false, or all trailing are unmatched (fine) */
            }
        }

        if (ctor_use_positional) {
            /* Count how many consecutive matched params from start */
            for (uint32_t p = 0; p < ctor_num_params; p++) {
                if (ctor_param_col[p] >= 0 || ctor_param_dot[p]) {
                    ctor_positional_count = p + 1;
                } else {
                    break;
                }
            }
            ctor_args = safe_emalloc(ctor_positional_count, sizeof(zval), 0);
        } else {
            zend_hash_init(&ctor_named_ht, ctor_num_params, NULL, ZVAL_PTR_DTOR, 0);
            ctor_ht_inited = true;
        }
    }

    /* Check if ALL columns are consumed by ctor args (skip post-ctor property writes) */
    if (col_is_ctor_arg) {
        ctor_all_cols = true;
        for (int c = 0; c < ncols; c++) {
            if (!col_is_ctor_arg[c]) { ctor_all_cols = false; break; }
        }
    }

    /* Pre-resolve ctor function pointer and param limit (loop-invariant) */
    zend_function *ctor_fn = has_ctor ? ce->constructor : NULL;
    uint32_t ctor_param_limit = ctor_use_positional ? ctor_positional_count : ctor_num_params;

    /* Pre-build call info for positional ctor path (avoids per-row struct init) */
    zval ctor_retval;
    zend_fcall_info ctor_fci;
    zend_fcall_info_cache ctor_fcc;
    if (has_ctor && ctor_use_positional) {
        ctor_fci.size = sizeof(ctor_fci);
        ZVAL_UNDEF(&ctor_fci.function_name);
        ctor_fci.retval = &ctor_retval;
        ctor_fci.param_count = ctor_param_limit;
        ctor_fci.params = ctor_args;
        ctor_fci.named_params = NULL;
        ctor_fci.object = NULL; /* set per-row */

        ctor_fcc.function_handler = ctor_fn;
        ctor_fcc.called_scope = ce;
        ctor_fcc.closure = NULL;
        ctor_fcc.object = NULL; /* set per-row */
    }

    /* Build return array */
    array_init_size(return_value, nrows);

    for (int r = 0; r < nrows; r++) {
        zval row_obj;
        if (object_init_ex(&row_obj, ce) != SUCCESS) {
            zend_throw_exception(zend_ce_error,
                "pg_fast_query(): failed to instantiate object", 0);
            goto cleanup;
        }

        /* ---- Constructor invocation ---- */
        if (has_ctor) {
            if (ctor_num_params > 0) {

                if (ctor_use_positional) {
                    /* --- Positional args fast path --- */
                    for (uint32_t p = 0; p < ctor_param_limit; p++) {
                        if (ctor_param_col[p] >= 0) {
                            int c = ctor_param_col[p];
                            bool is_null = PQgetisnull(res, r, c);

                            if (is_null) {
                                ZVAL_NULL(&ctor_args[p]);
                            } else {
                                convert_pg_value(&ctor_args[p], PQgetvalue(res, r, c),
                                                 PQgetlength(res, r, c), col_oids[c], datetime_ce);
                            }

                            if (ctor_param_nest_ce[p] && !is_null) {
                                zval nested_obj;
                                if (object_init_ex(&nested_obj, ctor_param_nest_ce[p]) == SUCCESS) {
                                    zval *slot = OBJ_PROP(Z_OBJ(nested_obj), ctor_param_nest_offset[p]);
                                    zval_ptr_dtor(slot);
                                    ZVAL_COPY_VALUE(slot, &ctor_args[p]);
                                    ZVAL_COPY_VALUE(&ctor_args[p], &nested_obj);
                                }
                            }
                        } else if (ctor_param_dot[p]) {
                            ctor_nest_info *ni = ctor_param_dot[p];
                            if (object_init_ex(&ctor_args[p], ni->ce) != SUCCESS) {
                                ZVAL_NULL(&ctor_args[p]);
                                continue;
                            }

                            for (uint32_t j = 0; j < ni->count; j++) {
                                int c = ni->props[j].col_idx;
                                zval pval;
                                bool col_null = PQgetisnull(res, r, c);

                                if (col_null) {
                                    ZVAL_NULL(&pval);
                                } else {
                                    convert_pg_value(&pval, PQgetvalue(res, r, c),
                                                     PQgetlength(res, r, c), col_oids[c], datetime_ce);
                                }

                                if (col_null ? ni->props[j].accepts_null : ni->props[j].skip_type_check) {
                                    zval *slot = OBJ_PROP(Z_OBJ(ctor_args[p]), ni->props[j].prop_offset);
                                    zval_ptr_dtor(slot);
                                    ZVAL_COPY_VALUE(slot, &pval);
                                } else {
                                    zval_ptr_dtor(&pval);
                                }
                            }
                        } else {
                            ZVAL_UNDEF(&ctor_args[p]);
                        }
                    }

                    ctor_fci.object = Z_OBJ(row_obj);
                    ctor_fcc.object = Z_OBJ(row_obj);
                    zend_call_function(&ctor_fci, &ctor_fcc);
                    zval_ptr_dtor(&ctor_retval);

                    for (uint32_t p = 0; p < ctor_param_limit; p++) {
                        zval_ptr_dtor(&ctor_args[p]);
                    }
                } else {
                    /* --- Named params fallback (gaps in param mapping) --- */
                    zend_hash_clean(&ctor_named_ht);

                    for (uint32_t p = 0; p < ctor_param_limit; p++) {
                        if (ctor_param_col[p] >= 0) {
                            int c = ctor_param_col[p];
                            zval val;
                            bool is_null = PQgetisnull(res, r, c);

                            if (is_null) {
                                ZVAL_NULL(&val);
                            } else {
                                convert_pg_value(&val, PQgetvalue(res, r, c),
                                                 PQgetlength(res, r, c), col_oids[c], datetime_ce);
                            }

                            if (ctor_param_nest_ce[p] && !is_null) {
                                zval nested_obj;
                                if (object_init_ex(&nested_obj, ctor_param_nest_ce[p]) == SUCCESS) {
                                    zval *slot = OBJ_PROP(Z_OBJ(nested_obj), ctor_param_nest_offset[p]);
                                    zval_ptr_dtor(slot);
                                    ZVAL_COPY_VALUE(slot, &val);
                                    ZVAL_COPY_VALUE(&val, &nested_obj);
                                }
                            }

                            zend_hash_add_new(&ctor_named_ht, ctor_fn->common.arg_info[p].name, &val);

                        } else if (ctor_param_dot[p]) {
                            ctor_nest_info *ni = ctor_param_dot[p];
                            zval nested_obj;
                            if (object_init_ex(&nested_obj, ni->ce) != SUCCESS) continue;

                            for (uint32_t j = 0; j < ni->count; j++) {
                                int c = ni->props[j].col_idx;
                                zval pval;
                                bool col_null = PQgetisnull(res, r, c);

                                if (col_null) {
                                    ZVAL_NULL(&pval);
                                } else {
                                    convert_pg_value(&pval, PQgetvalue(res, r, c),
                                                     PQgetlength(res, r, c), col_oids[c], datetime_ce);
                                }

                                if (col_null ? ni->props[j].accepts_null : ni->props[j].skip_type_check) {
                                    zval *slot = OBJ_PROP(Z_OBJ(nested_obj), ni->props[j].prop_offset);
                                    zval_ptr_dtor(slot);
                                    ZVAL_COPY_VALUE(slot, &pval);
                                } else {
                                    zval_ptr_dtor(&pval);
                                }
                            }

                            zend_hash_add_new(&ctor_named_ht, ctor_fn->common.arg_info[p].name, &nested_obj);
                        }
                    }

                    zend_call_known_function(ctor_fn, Z_OBJ(row_obj), ce, NULL,
                                             0, NULL, &ctor_named_ht);
                }
            } else {
                /* Constructor with zero params (may still call parent::__construct) */
                zend_call_known_function(ctor_fn, Z_OBJ(row_obj), ce, NULL,
                                         0, NULL, NULL);
            }

            if (UNEXPECTED(EG(exception))) {
                zval_ptr_dtor(&row_obj);
                goto cleanup;
            }
        }

        /* ---- Per-column property writes (skip if all consumed by ctor) ---- */
        if (ctor_all_cols) goto row_done;
        for (int c = 0; c < ncols; c++) {
            if (col_is_ctor_arg && col_is_ctor_arg[c]) continue;

            zval val;
            bool is_null = PQgetisnull(res, r, c);

            if (is_null) {
                ZVAL_NULL(&val);
            } else {
                convert_pg_value(&val, PQgetvalue(res, r, c),
                                 PQgetlength(res, r, c), col_oids[c], datetime_ce);
            }

            /* ---- Fast path: direct property slot writes with nested support ---- */
            if (class_fast_path && mappings[c].valid) {
                col_mapping *m = &mappings[c];

                if (m->depth == 1) {
                    if (is_null ? m->accepts_null : m->skip_type_check) {
                        zval *slot = OBJ_PROP(Z_OBJ(row_obj), m->offsets[0]);
                        zval_ptr_dtor(slot);
                        ZVAL_COPY_VALUE(slot, &val);
                        continue;
                    }
                    zend_update_property_ex(ce, Z_OBJ(row_obj), m->names[0], &val);
                    zval_ptr_dtor(&val);
                    continue;
                }

                /* Nested (depth > 1): walk path creating intermediate objects */
                zend_object *cur_obj = Z_OBJ(row_obj);
                bool ok = true;

                for (int i = 0; i < m->depth - 1; i++) {
                    zval *slot = OBJ_PROP(cur_obj, m->offsets[i]);
                    if (Z_TYPE_P(slot) != IS_OBJECT) {
                        zend_class_entry *child_ce = m->ces[i + 1];
                        zval_ptr_dtor(slot);
                        if (object_init_ex(slot, child_ce) != SUCCESS) {
                            ok = false;
                            break;
                        }
                    }
                    cur_obj = Z_OBJ_P(slot);
                }

                if (ok) {
                    int leaf = m->depth - 1;
                    if (is_null ? m->accepts_null : m->skip_type_check) {
                        zval *leaf_slot = OBJ_PROP(cur_obj, m->offsets[leaf]);
                        zval_ptr_dtor(leaf_slot);
                        ZVAL_COPY_VALUE(leaf_slot, &val);
                        continue;
                    }
                    zend_update_property_ex(m->ces[leaf], cur_obj, m->names[leaf], &val);
                    zval_ptr_dtor(&val);
                    continue;
                }
                zval_ptr_dtor(&val);
                continue;
            }

            /* ---- Slow path: full property write (hash lookup + type coercion) ---- */
            if (col_names) {
                zend_update_property_ex(ce, Z_OBJ(row_obj), col_names[c], &val);
            }
            zval_ptr_dtor(&val);
        }

row_done:
        add_next_index_zval(return_value, &row_obj);
    }

    /* ---- Cleanup ---- */
cleanup:
    if (mappings) {
        for (int c = 0; c < ncols; c++) free_col_mapping(&mappings[c]);
        efree(mappings);
    }
    if (col_names) {
        for (int c = 0; c < ncols; c++) zend_string_release(col_names[c]);
        efree(col_names);
    }
    efree(col_oids);

    if (ctor_ht_inited) zend_hash_destroy(&ctor_named_ht);
    if (ctor_args) efree(ctor_args);
    if (ctor_param_col) efree(ctor_param_col);
    if (col_is_ctor_arg) efree(col_is_ctor_arg);
    if (ctor_param_nest_ce) efree(ctor_param_nest_ce);
    if (ctor_param_nest_offset) efree(ctor_param_nest_offset);
    if (ctor_param_dot) {
        for (uint32_t p = 0; p < ctor_all_params; p++) {
            if (ctor_param_dot[p]) {
                efree(ctor_param_dot[p]->props);
                efree(ctor_param_dot[p]);
            }
        }
        efree(ctor_param_dot);
    }

    if (UNEXPECTED(EG(exception))) {
        RETURN_THROWS();
    }
}
/* }}} */

/* {{{ arginfo */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_pg_fast_query, 0, 2, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, result, IS_OBJECT, 0)
    ZEND_ARG_TYPE_INFO(0, class_name, IS_STRING, 0)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ function entries */
static const zend_function_entry pgsql_mapper_functions[] = {
    PHP_FE(pg_fast_query, arginfo_pg_fast_query)
    PHP_FE_END
};
/* }}} */

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(pgsql_mapper)
{
    return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(pgsql_mapper)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "pgsql_mapper support", "enabled");
    php_info_print_table_row(2, "Version", PHP_PGSQL_MAPPER_VERSION);
    php_info_print_table_end();
}
/* }}} */

/* {{{ pgsql_mapper_module_entry */
zend_module_entry pgsql_mapper_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_PGSQL_MAPPER_EXTNAME,
    pgsql_mapper_functions,
    PHP_MINIT(pgsql_mapper),
    NULL, /* MSHUTDOWN */
    NULL, /* RINIT */
    NULL, /* RSHUTDOWN */
    PHP_MINFO(pgsql_mapper),
    PHP_PGSQL_MAPPER_VERSION,
    STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_PGSQL_MAPPER
ZEND_GET_MODULE(pgsql_mapper)
#endif
