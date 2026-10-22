#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "ext/date/php_date.h"
#include "zend_exceptions.h"
#include "zend_enum.h"

#include "php_pgsql_mapper.h"

#define MAX_NESTING_DEPTH 8
#define NOT_MAPPED -1
#define INVALID_DIGITS -1

#define MIN_ISO_TIMESTAMPTZ_LENGTH 22
#define ISO_FRACTION_POSITION 19
#define ISO_SEPARATOR_COUNT 5
#define ISO_OFFSET_PART_COUNT 3
#define ISO_OFFSET_PART_WIDTH 2
#define ISO_BC_SUFFIX " BC"
#define ISO_BC_SUFFIX_LENGTH (sizeof(ISO_BC_SUFFIX) - 1)
#define MICROSECOND_DIGITS_SCALE 100000

#define SECONDS_PER_MINUTE 60
#define SECONDS_PER_HOUR 3600
#define SECONDS_PER_DAY 86400
#define DAYS_PER_400_YEARS 146097
#define DAYS_FROM_MARCH_YEAR_ZERO_TO_EPOCH 719468

#define NOT_INSTANTIABLE_CLASS_FLAGS (ZEND_ACC_INTERFACE | ZEND_ACC_TRAIT | ZEND_ACC_ENUM \
    | ZEND_ACC_IMPLICIT_ABSTRACT_CLASS | ZEND_ACC_EXPLICIT_ABSTRACT_CLASS)

typedef enum {
    FIELD_YEAR,
    FIELD_MONTH,
    FIELD_DAY,
    FIELD_HOUR,
    FIELD_MINUTE,
    FIELD_SECOND,
    ISO_FIELD_COUNT,
} iso_field_index;

typedef struct {
    int position;
    int width;
    int minimum;
    int maximum;
} iso_field;

typedef struct {
    int position;
    char separator;
} iso_separator;

static const iso_field ISO_DATE_TIME_FIELDS[ISO_FIELD_COUNT] = {
    {0, 4, 0, 9999},
    {5, 2, 1, 12},
    {8, 2, 1, 31},
    {11, 2, 0, 23},
    {14, 2, 0, 59},
    {17, 2, 0, 59},
};

static const iso_separator ISO_DATE_TIME_SEPARATORS[ISO_SEPARATOR_COUNT] = {
    {4, '-'},
    {7, '-'},
    {10, ' '},
    {13, ':'},
    {16, ':'},
};

static const int ISO_OFFSET_PART_SECONDS[ISO_OFFSET_PART_COUNT] = {
    SECONDS_PER_HOUR,
    SECONDS_PER_MINUTE,
    1,
};

typedef struct {
    int fields[ISO_FIELD_COUNT];
    int microsecond;
    int utc_offset;
} iso_timestamp;

typedef struct {
    bool skip_type_check;
    bool accepts_null;
} slot_type_check;

typedef struct {
    int depth;
    zend_string *names[MAX_NESTING_DEPTH];
    zend_class_entry *owner_classes[MAX_NESTING_DEPTH];
    zend_property_info *properties[MAX_NESTING_DEPTH];
    uint32_t property_offsets[MAX_NESTING_DEPTH];
    zend_class_entry *datetime_class;
    zend_class_entry *enum_class;
    slot_type_check type_check;
    bool valid;
} column_mapping;

typedef struct {
    Oid oid;
    zend_string *name;
    bool is_constructor_argument;
    bool skip_property_write;
    column_mapping mapping;
} column_plan;

typedef struct {
    int column;
    zend_property_info *property;
    zend_string *name;
    slot_type_check type_check;
} nested_column;

typedef struct {
    int column;
    zend_class_entry *nested_class;
    uint32_t nested_column_count;
    nested_column *nested_columns;
} constructor_param;

typedef struct {
    PGresult *result;
    zend_class_entry *target_class;
    zend_class_entry *default_datetime_class;
    bool is_fast_path;
    int column_count;
    column_plan *columns;
    bool all_columns_skipped;
    zend_function *constructor;
    uint32_t constructor_param_count;
    constructor_param *constructor_params;
    uint32_t argument_count;
    bool use_named_arguments;
    zval *positional_arguments;
    HashTable named_arguments;
    zval constructor_return;
    zend_fcall_info constructor_call;
    zend_fcall_info_cache constructor_call_cache;
} query_plan;

static zend_class_entry *pgsql_result_class;

static zend_class_entry *resolve_type_class(zend_type type)
{
    if (!ZEND_TYPE_HAS_NAME(type)) {
        return NULL;
    }

    return zend_lookup_class(ZEND_TYPE_NAME(type));
}

static bool is_fast_path_class(const zend_class_entry *class_entry)
{
    if (class_entry->__set != NULL) {
        return false;
    }

    return class_entry->create_object == NULL;
}

static bool is_nestable_class(zend_class_entry *class_entry)
{
    bool is_instantiable = (class_entry->ce_flags & NOT_INSTANTIABLE_CLASS_FLAGS) == 0;
    if (!is_instantiable) {
        return false;
    }

    bool is_date = instanceof_function(class_entry, php_date_get_interface_ce());
    if (is_date) {
        return false;
    }

    return is_fast_path_class(class_entry);
}

static zend_class_entry *resolve_nestable_class(zend_type type)
{
    zend_class_entry *class_entry = resolve_type_class(type);
    if (class_entry == NULL) {
        return NULL;
    }

    bool nestable = is_nestable_class(class_entry);

    return nestable ? class_entry : NULL;
}

static zend_class_entry *resolve_datetime_class(zend_type type, zend_class_entry *default_class)
{
    zend_class_entry *declared_class = resolve_type_class(type);
    if (declared_class == NULL) {
        return default_class;
    }

    bool is_instantiable = (declared_class->ce_flags & NOT_INSTANTIABLE_CLASS_FLAGS) == 0;
    if (!is_instantiable) {
        return default_class;
    }

    bool is_subclass = instanceof_function(declared_class, default_class);
    if (!is_subclass) {
        return default_class;
    }

    if (declared_class->create_object != default_class->create_object) {
        return default_class;
    }

    return declared_class;
}

static bool is_integer_oid(Oid oid)
{
    switch (oid) {
        case PG_OID_INT2:
        case PG_OID_INT4:
        case PG_OID_INT8:
            return true;
        default:
            return false;
    }
}

static bool is_timestamp_oid(Oid oid)
{
    switch (oid) {
        case PG_OID_TIMESTAMP:
        case PG_OID_TIMESTAMPTZ:
            return true;
        default:
            return false;
    }
}

static bool is_string_converted_oid(Oid oid)
{
    switch (oid) {
        case PG_OID_INT2:
        case PG_OID_INT4:
        case PG_OID_INT8:
        case PG_OID_FLOAT4:
        case PG_OID_FLOAT8:
        case PG_OID_BOOL:
        case PG_OID_TIMESTAMP:
        case PG_OID_TIMESTAMPTZ:
            return false;
        default:
            return true;
    }
}

static zend_class_entry *resolve_enum_class(zend_type type, Oid oid)
{
    zend_class_entry *declared_class = resolve_type_class(type);
    if (declared_class == NULL) {
        return NULL;
    }

    bool is_enum = (declared_class->ce_flags & ZEND_ACC_ENUM) != 0;
    if (!is_enum) {
        return NULL;
    }

    bool backing_matches = false;
    if (declared_class->enum_backing_type == IS_LONG) {
        backing_matches = is_integer_oid(oid);
    }
    if (declared_class->enum_backing_type == IS_STRING) {
        backing_matches = is_string_converted_oid(oid);
    }

    return backing_matches ? declared_class : NULL;
}

static bool is_writable_property(const zend_property_info *property)
{
    if (property->flags & ZEND_ACC_STATIC) {
        return false;
    }

    return property->hooks == NULL;
}

static zend_property_info *resolve_property(zend_class_entry *class_entry, zend_string *name)
{
    zend_property_info *property = zend_hash_find_ptr(&class_entry->properties_info, name);
    if (property == NULL) {
        return NULL;
    }

    bool writable = is_writable_property(property);

    return writable ? property : NULL;
}

static zend_property_info *get_first_property(zend_class_entry *class_entry, zend_string **name)
{
    zend_string *property_name;
    zend_property_info *property;

    ZEND_HASH_FOREACH_STR_KEY_PTR(&class_entry->properties_info, property_name, property) {
        bool writable = is_writable_property(property);
        if (writable) {
            *name = property_name;

            return property;
        }
    } ZEND_HASH_FOREACH_END();

    return NULL;
}

static bool accepts_instance_of(zend_type type, zend_class_entry *value_class)
{
    zend_class_entry *declared_class = resolve_type_class(type);
    if (declared_class == NULL) {
        return false;
    }

    return instanceof_function(value_class, declared_class);
}

static bool can_skip_type_check(zend_type type, Oid oid, zend_class_entry *datetime_class)
{
    if (!ZEND_TYPE_IS_SET(type)) {
        return true;
    }

    uint32_t mask = ZEND_TYPE_FULL_MASK(type);

    switch (oid) {
        case PG_OID_INT2:
        case PG_OID_INT4:
        case PG_OID_INT8:
            return (mask & MAY_BE_LONG) != 0;

        case PG_OID_FLOAT4:
        case PG_OID_FLOAT8:
            return (mask & MAY_BE_DOUBLE) != 0;

        case PG_OID_BOOL:
            return (mask & MAY_BE_BOOL) == MAY_BE_BOOL;

        case PG_OID_TIMESTAMP:
        case PG_OID_TIMESTAMPTZ:
            return accepts_instance_of(type, datetime_class);

        default:
            return (mask & MAY_BE_STRING) != 0;
    }
}

static bool type_accepts_null(zend_type type)
{
    if (!ZEND_TYPE_IS_SET(type)) {
        return true;
    }

    return (ZEND_TYPE_FULL_MASK(type) & MAY_BE_NULL) != 0;
}

static slot_type_check build_slot_type_check(zend_type type, Oid oid, zend_class_entry *datetime_class)
{
    slot_type_check type_check;
    type_check.skip_type_check = can_skip_type_check(type, oid, datetime_class);
    type_check.accepts_null = type_accepts_null(type);

    return type_check;
}

static zend_always_inline bool is_digit(char character)
{
    return character >= '0' && character <= '9';
}

static zend_always_inline zend_long parse_long(const char *digits)
{
    bool is_negative = digits[0] == '-';
    const char *cursor = is_negative ? digits + 1 : digits;
    zend_ulong magnitude = 0;

    for (; *cursor != '\0'; cursor++) {
        magnitude = magnitude * 10 + (zend_ulong)(*cursor - '0');
    }

    return (zend_long)(is_negative ? 0 - magnitude : magnitude);
}

static zend_always_inline double parse_float(const char *text)
{
    switch (text[0]) {
        case 'N':
            return ZEND_NAN;
        case 'I':
            return ZEND_INFINITY;
        case '-':
            if (text[1] == 'I') {
                return -ZEND_INFINITY;
            }
            break;
    }

    return zend_strtod(text, NULL);
}

static zend_always_inline int parse_digits(const char *text, int width)
{
    int value = 0;

    for (int index = 0; index < width; index++) {
        bool digit = is_digit(text[index]);
        if (!digit) {
            return INVALID_DIGITS;
        }
        value = value * 10 + (text[index] - '0');
    }

    return value;
}

static bool parse_iso_date_time(const char *raw, int fields[ISO_FIELD_COUNT])
{
#pragma GCC unroll 8
    for (int index = 0; index < ISO_SEPARATOR_COUNT; index++) {
        const iso_separator *expected = &ISO_DATE_TIME_SEPARATORS[index];
        if (raw[expected->position] != expected->separator) {
            return false;
        }
    }

#pragma GCC unroll 8
    for (int index = 0; index < ISO_FIELD_COUNT; index++) {
        const iso_field *field = &ISO_DATE_TIME_FIELDS[index];
        int value = parse_digits(raw + field->position, field->width);
        if (value == INVALID_DIGITS) {
            return false;
        }
        if (value < field->minimum) {
            return false;
        }
        if (value > field->maximum) {
            return false;
        }
        fields[index] = value;
    }

    return true;
}

static const char *parse_fraction(const char *cursor, const char *end, int *microsecond)
{
    *microsecond = 0;
    if (*cursor != '.') {
        return cursor;
    }

    cursor++;

    for (int scale = MICROSECOND_DIGITS_SCALE; scale > 0; scale /= 10) {
        if (cursor == end) {
            break;
        }
        bool digit = is_digit(*cursor);
        if (!digit) {
            break;
        }
        *microsecond += (*cursor - '0') * scale;
        cursor++;
    }

    return cursor;
}

static const char *parse_utc_offset(const char *cursor, const char *end, int *utc_offset)
{
    if (cursor == end) {
        return NULL;
    }

    int sign;
    switch (*cursor) {
        case '+':
            sign = 1;
            break;
        case '-':
            sign = -1;
            break;
        default:
            return NULL;
    }

    cursor++;
    int seconds = 0;

    for (int part = 0; part < ISO_OFFSET_PART_COUNT; part++) {
        ptrdiff_t remaining = end - cursor;
        if (remaining < ISO_OFFSET_PART_WIDTH) {
            return NULL;
        }

        int value = parse_digits(cursor, ISO_OFFSET_PART_WIDTH);
        if (value == INVALID_DIGITS) {
            return NULL;
        }

        seconds += value * ISO_OFFSET_PART_SECONDS[part];
        cursor += ISO_OFFSET_PART_WIDTH;

        if (cursor == end) {
            break;
        }
        if (*cursor != ':') {
            break;
        }
        cursor++;
    }

    *utc_offset = sign * seconds;

    return cursor;
}

static bool apply_era_suffix(const char *cursor, const char *end, int *year)
{
    if (cursor == end) {
        return true;
    }

    size_t remaining = end - cursor;
    if (remaining != ISO_BC_SUFFIX_LENGTH) {
        return false;
    }

    int compared = memcmp(cursor, ISO_BC_SUFFIX, ISO_BC_SUFFIX_LENGTH);
    if (compared != 0) {
        return false;
    }

    *year = 1 - *year;

    return true;
}

static bool parse_iso_timestamptz(const char *raw, int length, iso_timestamp *timestamp)
{
    if (length < MIN_ISO_TIMESTAMPTZ_LENGTH) {
        return false;
    }

    bool has_date_time = parse_iso_date_time(raw, timestamp->fields);
    if (!has_date_time) {
        return false;
    }

    const char *end = raw + length;
    const char *cursor = parse_fraction(raw + ISO_FRACTION_POSITION, end, &timestamp->microsecond);
    cursor = parse_utc_offset(cursor, end, &timestamp->utc_offset);
    if (cursor == NULL) {
        return false;
    }

    return apply_era_suffix(cursor, end, &timestamp->fields[FIELD_YEAR]);
}

static zend_long count_days_since_epoch(zend_long year, zend_long month, zend_long day)
{
    zend_long march_year = month <= 2 ? year - 1 : year;
    zend_long era = (march_year >= 0 ? march_year : march_year - 399) / 400;
    zend_long year_of_era = march_year - era * 400;
    zend_long march_month = month > 2 ? month - 3 : month + 9;
    zend_long day_of_year = (153 * march_month + 2) / 5 + day - 1;
    zend_long day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

    return era * DAYS_PER_400_YEARS + day_of_era - DAYS_FROM_MARCH_YEAR_ZERO_TO_EPOCH;
}

static void initialize_offset_time(php_date_obj *date, const iso_timestamp *timestamp)
{
    const int *fields = timestamp->fields;
    zend_long days = count_days_since_epoch(fields[FIELD_YEAR], fields[FIELD_MONTH], fields[FIELD_DAY]);
    zend_long local_seconds = days * SECONDS_PER_DAY
        + fields[FIELD_HOUR] * SECONDS_PER_HOUR
        + fields[FIELD_MINUTE] * SECONDS_PER_MINUTE
        + fields[FIELD_SECOND];

    php_date_initialize_from_ts_long(date, local_seconds - timestamp->utc_offset, timestamp->microsecond);

    timelib_time *time = date->time;
    time->y = fields[FIELD_YEAR];
    time->m = fields[FIELD_MONTH];
    time->d = fields[FIELD_DAY];
    time->h = fields[FIELD_HOUR];
    time->i = fields[FIELD_MINUTE];
    time->s = fields[FIELD_SECOND];
    time->z = timestamp->utc_offset;
    time->dst = 0;
    time->have_zone = 1;
    time->is_localtime = 1;
}

static void convert_pg_datetime(zval *value, const char *raw, int length, zend_class_entry *datetime_class)
{
    zval date_object;
    php_date_instantiate(datetime_class, &date_object);
    php_date_obj *date = Z_PHPDATE_P(&date_object);

    iso_timestamp timestamp;
    bool is_iso = parse_iso_timestamptz(raw, length, &timestamp);
    if (is_iso) {
        initialize_offset_time(date, &timestamp);
        ZVAL_COPY_VALUE(value, &date_object);

        return;
    }

    bool parsed = php_date_initialize(date, raw, length, NULL, NULL, 0);
    if (!parsed) {
        zval_ptr_dtor(&date_object);
        ZVAL_NULL(value);

        return;
    }

    ZVAL_COPY_VALUE(value, &date_object);
}

static bool convert_pg_enum(zval *value, const char *raw, int length, zend_class_entry *enum_class)
{
    zend_object *case_object = NULL;
    zend_result lookup_status;

    if (enum_class->enum_backing_type == IS_LONG) {
        lookup_status = zend_enum_get_case_by_value(&case_object, enum_class, parse_long(raw), NULL, false);
    } else {
        zend_string *key = zend_string_init(raw, length, 0);
        lookup_status = zend_enum_get_case_by_value(&case_object, enum_class, 0, key, false);
        zend_string_release(key);
    }

    if (lookup_status == FAILURE) {
        ZVAL_UNDEF(value);

        return false;
    }

    ZVAL_OBJ_COPY(value, case_object);

    return true;
}

static zend_always_inline void convert_pg_value(zval *value, const char *raw, int length,
                                                Oid oid, zend_class_entry *datetime_class)
{
    switch (oid) {
        case PG_OID_INT2:
        case PG_OID_INT4:
        case PG_OID_INT8:
            ZVAL_LONG(value, parse_long(raw));
            break;

        case PG_OID_FLOAT4:
        case PG_OID_FLOAT8:
            ZVAL_DOUBLE(value, parse_float(raw));
            break;

        case PG_OID_BOOL:
            ZVAL_BOOL(value, raw[0] == 't');
            break;

        case PG_OID_TIMESTAMP:
        case PG_OID_TIMESTAMPTZ:
            convert_pg_datetime(value, raw, length, datetime_class);
            break;

        default:
            ZVAL_STRINGL(value, raw, length);
            break;
    }
}

static zend_always_inline void read_cell(zval *value, PGresult *result, int row, int column,
                                         Oid oid, zend_class_entry *datetime_class)
{
    bool is_null = PQgetisnull(result, row, column);
    if (is_null) {
        ZVAL_NULL(value);

        return;
    }

    const char *raw = PQgetvalue(result, row, column);
    int length = PQgetlength(result, row, column);
    convert_pg_value(value, raw, length, oid, datetime_class);
}

static zend_always_inline bool read_mapped_cell(zval *value, PGresult *result, const column_plan *column_info,
                                                int row, int column)
{
    zend_class_entry *enum_class = column_info->mapping.enum_class;
    if (enum_class == NULL) {
        read_cell(value, result, row, column, column_info->oid, column_info->mapping.datetime_class);

        return true;
    }

    bool is_null = PQgetisnull(result, row, column);
    if (is_null) {
        ZVAL_NULL(value);

        return true;
    }

    const char *raw = PQgetvalue(result, row, column);
    int length = PQgetlength(result, row, column);

    return convert_pg_enum(value, raw, length, enum_class);
}

static zend_always_inline void write_property_value(zend_object *object, uint32_t offset, zend_property_info *property,
                                                    zend_string *name, zval *value, slot_type_check type_check)
{
    bool is_null_value = Z_TYPE_P(value) == IS_NULL;
    bool can_write_directly = is_null_value ? type_check.accepts_null : type_check.skip_type_check;
    if (can_write_directly) {
        zval *slot = OBJ_PROP(object, offset);
        i_zval_ptr_dtor(slot);
        ZVAL_COPY_VALUE(slot, value);

        return;
    }

    zend_update_property_ex(property->ce, object, name, value);
    zval_ptr_dtor(value);
}

static zend_object *get_or_create_nested_object(zend_object *owner, const column_mapping *mapping, int level)
{
    zval *slot = OBJ_PROP(owner, mapping->property_offsets[level]);
    if (Z_TYPE_P(slot) == IS_OBJECT) {
        return Z_OBJ_P(slot);
    }

    zval_ptr_dtor(slot);
    zend_result created = object_init_ex(slot, mapping->owner_classes[level + 1]);
    if (created != SUCCESS) {
        return NULL;
    }

    return Z_OBJ_P(slot);
}

static zend_always_inline void write_mapped_value(zend_object *object, const column_mapping *mapping, zval *value)
{
    int leaf = mapping->depth - 1;

    for (int level = 0; level < leaf; level++) {
        object = get_or_create_nested_object(object, mapping, level);
        if (object == NULL) {
            zval_ptr_dtor(value);

            return;
        }
    }

    write_property_value(object, mapping->property_offsets[leaf], mapping->properties[leaf], mapping->names[leaf],
                         value, mapping->type_check);
}

static bool resolve_property_path(column_mapping *mapping, const char *column_name, zend_class_entry *root_class)
{
    const char *segment = column_name;
    zend_class_entry *owner_class = root_class;

    for (int level = 0; level < MAX_NESTING_DEPTH; level++) {
        const char *dot = strchr(segment, '.');
        size_t length = dot == NULL ? strlen(segment) : (size_t)(dot - segment);
        if (length == 0) {
            return false;
        }

        zend_string *name = zend_string_init(segment, length, 0);
        mapping->names[level] = name;
        mapping->owner_classes[level] = owner_class;

        zend_property_info *property = resolve_property(owner_class, name);
        if (property == NULL) {
            return false;
        }
        mapping->properties[level] = property;
        mapping->property_offsets[level] = property->offset;

        if (dot == NULL) {
            mapping->depth = level + 1;

            return true;
        }

        owner_class = resolve_nestable_class(property->type);
        if (owner_class == NULL) {
            return false;
        }
        segment = dot + 1;
    }

    return false;
}

static bool expand_single_value_object(column_mapping *mapping)
{
    zend_class_entry *value_class = resolve_nestable_class(mapping->properties[0]->type);
    if (value_class == NULL) {
        return true;
    }

    zend_string *first_name;
    zend_property_info *first_property = get_first_property(value_class, &first_name);
    if (first_property == NULL) {
        return false;
    }

    mapping->depth = 2;
    mapping->names[1] = zend_string_copy(first_name);
    mapping->owner_classes[1] = value_class;
    mapping->properties[1] = first_property;
    mapping->property_offsets[1] = first_property->offset;

    return true;
}

static void resolve_leaf_conversion(column_mapping *mapping, Oid oid)
{
    zend_property_info *leaf = mapping->properties[mapping->depth - 1];

    bool is_timestamp = is_timestamp_oid(oid);
    if (is_timestamp) {
        mapping->datetime_class = resolve_datetime_class(leaf->type, mapping->datetime_class);
    }

    mapping->enum_class = resolve_enum_class(leaf->type, oid);
    mapping->type_check = build_slot_type_check(leaf->type, oid, mapping->datetime_class);
    if (mapping->enum_class != NULL) {
        mapping->type_check.skip_type_check = true;
    }
}

static void build_column_mapping(column_mapping *mapping, const char *column_name, zend_class_entry *root_class, Oid oid)
{
    bool resolved = resolve_property_path(mapping, column_name, root_class);
    if (!resolved) {
        return;
    }

    if (mapping->depth == 1) {
        bool expanded = expand_single_value_object(mapping);
        if (!expanded) {
            return;
        }
    }

    resolve_leaf_conversion(mapping, oid);
    mapping->valid = true;
}

static void plan_columns(query_plan *plan)
{
    plan->columns = ecalloc(plan->column_count, sizeof(column_plan));

    for (int column = 0; column < plan->column_count; column++) {
        column_plan *column_info = &plan->columns[column];
        const char *name = PQfname(plan->result, column);
        column_info->oid = PQftype(plan->result, column);
        column_info->mapping.datetime_class = plan->default_datetime_class;

        if (plan->is_fast_path) {
            build_column_mapping(&column_info->mapping, name, plan->target_class, column_info->oid);
            continue;
        }

        column_info->name = zend_new_interned_string(zend_string_init(name, strlen(name), 0));
    }
}

static bool has_mapped_column_for(const query_plan *plan, const zend_property_info *property)
{
    for (int column = 0; column < plan->column_count; column++) {
        const column_mapping *mapping = &plan->columns[column].mapping;
        if (!mapping->valid) {
            continue;
        }
        if (mapping->properties[0] == property) {
            return true;
        }
    }

    return false;
}

static bool is_promoted_param_with_column(const query_plan *plan, const zend_function *constructor,
                                          const zend_arg_info *argument)
{
    if (!ZEND_ARG_IS_PROMOTED(argument)) {
        return false;
    }

    zend_property_info *property = resolve_property(plan->target_class, argument->name);
    if (property == NULL) {
        return false;
    }

    if (property->ce != constructor->common.scope) {
        return false;
    }

    return has_mapped_column_for(plan, property);
}

static bool has_only_promoted_assignments(const zend_op_array *op_array, uint32_t param_count)
{
    uint32_t assignments = 0;

    for (uint32_t index = 0; index < op_array->last; index++) {
        switch (op_array->opcodes[index].opcode) {
            case ZEND_ASSIGN_OBJ:
                assignments++;
                continue;
            case ZEND_RECV:
            case ZEND_RECV_INIT:
            case ZEND_OP_DATA:
            case ZEND_RETURN:
            case ZEND_NOP:
            case ZEND_EXT_STMT:
            case ZEND_EXT_NOP:
                continue;
            default:
                return false;
        }
    }

    return assignments == param_count;
}

static bool can_skip_promoted_constructor(const query_plan *plan, const zend_function *constructor)
{
    if (constructor->type != ZEND_USER_FUNCTION) {
        return false;
    }

    if (constructor->common.fn_flags & ZEND_ACC_VARIADIC) {
        return false;
    }

    uint32_t param_count = constructor->common.num_args;
    if (param_count == 0) {
        return false;
    }

    for (uint32_t index = 0; index < param_count; index++) {
        bool skippable = is_promoted_param_with_column(plan, constructor, &constructor->common.arg_info[index]);
        if (!skippable) {
            return false;
        }
    }

    return has_only_promoted_assignments(&constructor->op_array, param_count);
}

static void initialize_nested_column(nested_column *nested, const query_plan *plan, int column,
                                     zend_property_info *property, zend_string *name)
{
    Oid oid = plan->columns[column].oid;

    nested->column = column;
    nested->property = property;
    nested->name = name;
    nested->type_check = build_slot_type_check(property->type, oid, plan->default_datetime_class);
}

static int find_unconsumed_column(const query_plan *plan, zend_string *name)
{
    for (int column = 0; column < plan->column_count; column++) {
        if (plan->columns[column].is_constructor_argument) {
            continue;
        }

        const char *column_name = PQfname(plan->result, column);
        bool matches = zend_string_equals_cstr(name, column_name, strlen(column_name));
        if (matches) {
            return column;
        }
    }

    return NOT_MAPPED;
}

static void match_direct_column(query_plan *plan, constructor_param *param, const zend_arg_info *argument)
{
    int column = find_unconsumed_column(plan, argument->name);
    if (column == NOT_MAPPED) {
        return;
    }

    param->column = column;
    plan->columns[column].is_constructor_argument = true;

    zend_class_entry *nested_class = resolve_nestable_class(argument->type);
    if (nested_class == NULL) {
        return;
    }

    zend_string *first_name;
    zend_property_info *first_property = get_first_property(nested_class, &first_name);
    if (first_property == NULL) {
        return;
    }

    nested_column *nested = emalloc(sizeof(nested_column));
    initialize_nested_column(nested, plan, column, first_property, zend_string_copy(first_name));

    param->nested_class = nested_class;
    param->nested_columns = nested;
    param->nested_column_count = 1;
}

static const char *get_dot_path_rest(const char *column_name, const zend_string *prefix)
{
    size_t prefix_length = ZSTR_LEN(prefix);

    int compared = strncmp(column_name, ZSTR_VAL(prefix), prefix_length);
    if (compared != 0) {
        return NULL;
    }

    if (column_name[prefix_length] != '.') {
        return NULL;
    }

    const char *rest = column_name + prefix_length + 1;
    if (*rest == '\0') {
        return NULL;
    }

    return rest;
}

static void collect_dot_columns(query_plan *plan, constructor_param *param, const zend_arg_info *argument,
                                zend_class_entry *nested_class)
{
    nested_column *found = safe_emalloc(plan->column_count, sizeof(nested_column), 0);
    uint32_t count = 0;

    for (int column = 0; column < plan->column_count; column++) {
        column_plan *column_info = &plan->columns[column];
        if (column_info->is_constructor_argument) {
            continue;
        }

        const char *rest = get_dot_path_rest(PQfname(plan->result, column), argument->name);
        if (rest == NULL) {
            continue;
        }

        zend_string *name = zend_string_init(rest, strlen(rest), 0);
        zend_property_info *property = resolve_property(nested_class, name);
        if (property == NULL) {
            zend_string_release(name);
            continue;
        }

        initialize_nested_column(&found[count], plan, column, property, name);
        column_info->is_constructor_argument = true;
        count++;
    }

    if (count == 0) {
        efree(found);

        return;
    }

    param->nested_class = nested_class;
    param->nested_columns = found;
    param->nested_column_count = count;
}

static void match_constructor_params(query_plan *plan)
{
    const zend_arg_info *arguments = plan->constructor->common.arg_info;

    for (uint32_t index = 0; index < plan->constructor_param_count; index++) {
        plan->constructor_params[index].column = NOT_MAPPED;
        match_direct_column(plan, &plan->constructor_params[index], &arguments[index]);
    }

    for (uint32_t index = 0; index < plan->constructor_param_count; index++) {
        constructor_param *param = &plan->constructor_params[index];
        if (param->column != NOT_MAPPED) {
            continue;
        }

        zend_class_entry *nested_class = resolve_nestable_class(arguments[index].type);
        if (nested_class == NULL) {
            continue;
        }

        collect_dot_columns(plan, param, &arguments[index], nested_class);
    }
}

static bool is_param_mapped(const constructor_param *param)
{
    if (param->column != NOT_MAPPED) {
        return true;
    }

    return param->nested_class != NULL;
}

static uint32_t count_leading_mapped_params(const query_plan *plan)
{
    uint32_t count = 0;

    while (count < plan->constructor_param_count) {
        bool mapped = is_param_mapped(&plan->constructor_params[count]);
        if (!mapped) {
            break;
        }
        count++;
    }

    return count;
}

static bool has_mapped_param_from(const query_plan *plan, uint32_t start)
{
    for (uint32_t index = start; index < plan->constructor_param_count; index++) {
        bool mapped = is_param_mapped(&plan->constructor_params[index]);
        if (mapped) {
            return true;
        }
    }

    return false;
}

static void plan_constructor_arguments(query_plan *plan)
{
    uint32_t leading_count = count_leading_mapped_params(plan);
    plan->use_named_arguments = has_mapped_param_from(plan, leading_count);

    if (plan->use_named_arguments) {
        plan->argument_count = plan->constructor_param_count;
        zend_hash_init(&plan->named_arguments, plan->constructor_param_count, NULL, ZVAL_PTR_DTOR, 0);

        return;
    }

    plan->argument_count = leading_count;
    plan->positional_arguments = safe_emalloc(leading_count, sizeof(zval), 0);
}

static void plan_constructor_params(query_plan *plan)
{
    if (plan->constructor->type != ZEND_USER_FUNCTION) {
        return;
    }

    plan->constructor_param_count = plan->constructor->common.num_args;
    if (plan->constructor_param_count == 0) {
        return;
    }

    plan->constructor_params = ecalloc(plan->constructor_param_count, sizeof(constructor_param));
    match_constructor_params(plan);
    plan_constructor_arguments(plan);
}

static void prepare_constructor_call(query_plan *plan)
{
    zend_fcall_info *call = &plan->constructor_call;
    call->size = sizeof(zend_fcall_info);
    ZVAL_UNDEF(&call->function_name);
    call->retval = &plan->constructor_return;
    call->param_count = plan->use_named_arguments ? 0 : plan->argument_count;
    call->params = plan->positional_arguments;
    call->named_params = plan->use_named_arguments ? &plan->named_arguments : NULL;

    zend_fcall_info_cache *cache = &plan->constructor_call_cache;
    cache->function_handler = plan->constructor;
    cache->called_scope = plan->target_class;
    cache->closure = NULL;
}

static void plan_constructor(query_plan *plan)
{
    zend_function *constructor = plan->target_class->constructor;
    if (constructor == NULL) {
        return;
    }

    if (plan->is_fast_path) {
        bool skippable = can_skip_promoted_constructor(plan, constructor);
        if (skippable) {
            return;
        }
    }

    plan->constructor = constructor;
    plan_constructor_params(plan);
    prepare_constructor_call(plan);
}

static bool should_skip_property_write(const query_plan *plan, const column_plan *column_info)
{
    if (column_info->is_constructor_argument) {
        return true;
    }

    if (!plan->is_fast_path) {
        return false;
    }

    return !column_info->mapping.valid;
}

static void plan_property_writes(query_plan *plan)
{
    plan->all_columns_skipped = true;

    for (int column = 0; column < plan->column_count; column++) {
        column_plan *column_info = &plan->columns[column];
        column_info->skip_property_write = should_skip_property_write(plan, column_info);
        if (!column_info->skip_property_write) {
            plan->all_columns_skipped = false;
        }
    }
}

static void build_query_plan(query_plan *plan, PGresult *result, zend_class_entry *target_class)
{
    memset(plan, 0, sizeof(*plan));
    plan->result = result;
    plan->target_class = target_class;
    plan->default_datetime_class = php_date_get_immutable_ce();
    plan->is_fast_path = is_fast_path_class(target_class);
    plan->column_count = PQnfields(result);

    plan_columns(plan);
    plan_constructor(plan);
    plan_property_writes(plan);
}

static void free_column_plan(column_plan *column_info)
{
    for (int level = 0; level < MAX_NESTING_DEPTH; level++) {
        zend_string *name = column_info->mapping.names[level];
        if (name != NULL) {
            zend_string_release(name);
        }
    }

    if (column_info->name != NULL) {
        zend_string_release(column_info->name);
    }
}

static void free_constructor_param(constructor_param *param)
{
    for (uint32_t index = 0; index < param->nested_column_count; index++) {
        zend_string_release(param->nested_columns[index].name);
    }

    if (param->nested_columns != NULL) {
        efree(param->nested_columns);
    }
}

static void free_query_plan(query_plan *plan)
{
    for (int column = 0; column < plan->column_count; column++) {
        free_column_plan(&plan->columns[column]);
    }
    efree(plan->columns);

    if (plan->constructor_params != NULL) {
        for (uint32_t index = 0; index < plan->constructor_param_count; index++) {
            free_constructor_param(&plan->constructor_params[index]);
        }
        efree(plan->constructor_params);
    }

    if (plan->positional_arguments != NULL) {
        efree(plan->positional_arguments);
    }

    if (plan->use_named_arguments) {
        zend_hash_destroy(&plan->named_arguments);
    }
}

static void fill_nested_object(zend_object *object, const query_plan *plan, const constructor_param *param, int row)
{
    for (uint32_t index = 0; index < param->nested_column_count; index++) {
        const nested_column *nested = &param->nested_columns[index];
        zval value;
        read_cell(&value, plan->result, row, nested->column, plan->columns[nested->column].oid,
                  plan->default_datetime_class);
        write_property_value(object, nested->property->offset, nested->property, nested->name, &value,
                             nested->type_check);
    }
}

static zend_always_inline void build_constructor_argument(zval *argument, const query_plan *plan, const constructor_param *param, int row)
{
    if (param->nested_class == NULL) {
        read_cell(argument, plan->result, row, param->column, plan->columns[param->column].oid,
                  plan->default_datetime_class);

        return;
    }

    if (param->column != NOT_MAPPED) {
        bool is_null = PQgetisnull(plan->result, row, param->column);
        if (is_null) {
            ZVAL_NULL(argument);

            return;
        }
    }

    zend_result created = object_init_ex(argument, param->nested_class);
    if (created != SUCCESS) {
        ZVAL_NULL(argument);

        return;
    }

    fill_nested_object(Z_OBJ_P(argument), plan, param, row);
}

static void invoke_constructor(zend_object *object, query_plan *plan)
{
    plan->constructor_call.object = object;
    plan->constructor_call_cache.object = object;
    zend_call_function(&plan->constructor_call, &plan->constructor_call_cache);
    zval_ptr_dtor(&plan->constructor_return);
}

static void call_constructor_with_named_arguments(zend_object *object, query_plan *plan, int row)
{
    zend_hash_clean(&plan->named_arguments);

    for (uint32_t index = 0; index < plan->argument_count; index++) {
        const constructor_param *param = &plan->constructor_params[index];
        bool mapped = is_param_mapped(param);
        if (!mapped) {
            continue;
        }

        zval argument;
        build_constructor_argument(&argument, plan, param, row);
        zend_hash_add_new(&plan->named_arguments, plan->constructor->common.arg_info[index].name, &argument);
    }

    invoke_constructor(object, plan);
}

static void call_constructor(zend_object *object, query_plan *plan, int row)
{
    if (plan->use_named_arguments) {
        call_constructor_with_named_arguments(object, plan, row);

        return;
    }

    for (uint32_t index = 0; index < plan->argument_count; index++) {
        build_constructor_argument(&plan->positional_arguments[index], plan, &plan->constructor_params[index], row);
    }

    invoke_constructor(object, plan);

    for (uint32_t index = 0; index < plan->argument_count; index++) {
        zval_ptr_dtor(&plan->positional_arguments[index]);
    }
}

static void write_columns(zend_object *object, const query_plan *plan, int row)
{
    PGresult *result = plan->result;
    const column_plan *columns = plan->columns;
    int column_count = plan->column_count;
    bool is_fast_path = plan->is_fast_path;

    for (int column = 0; column < column_count; column++) {
        const column_plan *column_info = &columns[column];
        if (column_info->skip_property_write) {
            continue;
        }

        zval value;
        bool converted = read_mapped_cell(&value, result, column_info, row, column);
        if (!converted) {
            return;
        }

        if (is_fast_path) {
            write_mapped_value(object, &column_info->mapping, &value);
            continue;
        }

        zend_update_property_ex(plan->target_class, object, column_info->name, &value);
        zval_ptr_dtor(&value);
    }
}

static bool discard_if_failed(zval *row_object)
{
    if (EG(exception) == NULL) {
        return false;
    }

    zval_ptr_dtor(row_object);

    return true;
}

static bool hydrate_row(zval *row_object, query_plan *plan, int row)
{
    zend_result created = object_init_ex(row_object, plan->target_class);
    if (created != SUCCESS) {
        return false;
    }

    if (plan->constructor != NULL) {
        call_constructor(Z_OBJ_P(row_object), plan, row);
        bool constructor_failed = discard_if_failed(row_object);
        if (constructor_failed) {
            return false;
        }
    }

    if (!plan->all_columns_skipped) {
        write_columns(Z_OBJ_P(row_object), plan, row);
    }

    bool write_failed = discard_if_failed(row_object);

    return !write_failed;
}

PHP_FUNCTION(pg_fast_query)
{
    zval *result_object;
    zend_class_entry *target_class = NULL;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(result_object, pgsql_result_class)
        Z_PARAM_CLASS(target_class)
    ZEND_PARSE_PARAMETERS_END();

    PGresult *result = pgsql_result_from_obj(Z_OBJ_P(result_object))->result;
    if (result == NULL) {
        zend_throw_error(NULL, "pg_fast_query(): result handle is invalid");
        RETURN_THROWS();
    }

    ExecStatusType status = PQresultStatus(result);
    if (status != PGRES_TUPLES_OK) {
        zend_throw_exception_ex(zend_ce_exception, 0,
            "pg_fast_query(): result does not contain rows (status: %s)", PQresStatus(status));
        RETURN_THROWS();
    }

    query_plan plan;
    build_query_plan(&plan, result, target_class);

    int row_count = PQntuples(result);
    array_init_size(return_value, row_count);

    for (int row = 0; row < row_count; row++) {
        zval row_object;
        bool hydrated = hydrate_row(&row_object, &plan, row);
        if (!hydrated) {
            break;
        }
        add_next_index_zval(return_value, &row_object);
    }

    free_query_plan(&plan);

    if (EG(exception) != NULL) {
        RETURN_THROWS();
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_pg_fast_query, 0, 2, IS_ARRAY, 0)
    ZEND_ARG_OBJ_INFO(0, result, PgSql\\Result, 0)
    ZEND_ARG_TYPE_INFO(0, class, IS_STRING, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry pgsql_mapper_functions[] = {
    PHP_FE(pg_fast_query, arginfo_pg_fast_query)
    PHP_FE_END
};

static const zend_module_dep pgsql_mapper_dependencies[] = {
    ZEND_MOD_REQUIRED("pgsql")
    ZEND_MOD_REQUIRED("date")
    ZEND_MOD_END
};

PHP_MINIT_FUNCTION(pgsql_mapper)
{
    pgsql_result_class = zend_hash_str_find_ptr(CG(class_table), "pgsql\\result", sizeof("pgsql\\result") - 1);

    return SUCCESS;
}

PHP_MINFO_FUNCTION(pgsql_mapper)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "pgsql_mapper support", "enabled");
    php_info_print_table_row(2, "Version", PHP_PGSQL_MAPPER_VERSION);
    php_info_print_table_end();
}

zend_module_entry pgsql_mapper_module_entry = {
    STANDARD_MODULE_HEADER_EX,
    NULL,
    pgsql_mapper_dependencies,
    PHP_PGSQL_MAPPER_EXTNAME,
    pgsql_mapper_functions,
    PHP_MINIT(pgsql_mapper),
    NULL,
    NULL,
    NULL,
    PHP_MINFO(pgsql_mapper),
    PHP_PGSQL_MAPPER_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_PGSQL_MAPPER
ZEND_GET_MODULE(pgsql_mapper)
#endif
