# pgsql_mapper Development Guide

## Overview
A PHP C extension that hydrates PostgreSQL results directly into typed PHP objects.
**Signature:** `pg_fast_query(PgSql\Result $result, string $class): array`

## Project & Build
* **Core Logic:** `pgsql_mapper.c` (main extension) and `php_pgsql_mapper.h` (vendored PgSql structs & OIDs).
* **Build:** Run `bash build-install.sh` for a full rebuild/install. (Run `phpize && ./configure` first if `config.m4` changes.) Needs system libpq and libyyjson (found via pkg-config); PIE can't install either.
* **Tests:** Run `php test.php` (Requires local PG at `127.0.0.1:15432`, db=`postgres_air`, user=`app`).

## Architecture
* **Execution Paths:**
    * **Fast:** Direct slot writes for classes without `__set` or custom object handlers, any property visibility. A type mismatch falls back to `zend_update_property_ex()` with the declaring class as scope.
    * **Promoted-only constructor:** Skipped (`can_skip_promoted_constructor()`) when every param is promoted, declared by the constructor's class, has a column, and the body has no other code; the fast path fills the slots.
    * **Constructor:** Called through the VM with positional or named args. Internal (C) constructors are called without args.
    * **Slow:** `zend_update_property_ex()` for classes with `__set` or `create_object`.
* **Object Mapping:**
    * **Dot Notation:** Columns like `"address.city"` map to `$obj->address->city` (max depth: 8). Instances are reused if referenced multiple times.
    * **Auto-nesting:** If a column targets a user class, the extension auto-creates the object and sets the value on its **first declared property**.
* **Type Conversion:** PostgreSQL OIDs map to `int`, `float` (incl. NaN/±INF), `bool`, `DateTimeImmutable` (timestamps), and `string` (fallback). On the fast path, `resolve_conversion()` picks a type-directed `column_conversion` from the property type: backed enum, `date` → UTC midnight, `json`/`jsonb` → array (yyjson, `json_decode` semantics), PG array text → list (`parse_pg_array()`). A property that accepts `string` keeps the raw value.
* **Direct writes:** `can_write_directly()` decides per value: `null` → `accepts_null`, array → `accepts_array`, anything else → `skip_type_check`. JSON/array conversions never skip the check, so a scalar JSON value goes through normal assignment.
* **Per-call plan:** `build_query_plan()` resolves columns, constructor params and the constructor call once; `hydrate_row()` only reads cells and writes slots.
* **Timestamps:** `parse_iso_timestamptz()` + `initialize_offset_time()` build ISO-style `timestamptz` values directly (no strtotime). Other shapes (`timestamp` without zone, non-ISO `DateStyle`, `infinity`) go through `php_date_initialize()`.
* **Vendored Structs:** The internal structs in `php_pgsql_mapper.h` **must** match the PHP version being compiled against.

## Adding a New Type Mapping
1. Define the OID constant in `php_pgsql_mapper.h`.
2. Default conversion: add a `case` to `convert_pg_value()` and `can_skip_type_check()`. Type-directed: add a `column_conversion`, pick it in `resolve_conversion()`, handle it in `convert_mapped_value()` and `can_skip_conversion_type_check()`.
3. Element of PG arrays: add the array OID to `get_array_element_oid()`.
4. Write a test case in `test.php`.

## Critical Pitfalls
* **Lifecycle:** **Do NOT** `PQclear` the result; PHP's ext/pgsql owns it.
* **Memory:** Always run `zval_ptr_dtor` before overwriting property slots to free old values.
* **Properties:** Always skip `ZEND_ACC_STATIC` and PHP 8.4+ hooked properties.
* **Null checks:** Decide slot writes on the converted value (`Z_TYPE(val) == IS_NULL`), not on `PQgetisnull` — a failed conversion yields null too.
* **Symbols:** `timelib_*` is not exported from the PHP binary; use only `PHPAPI` date functions.
* **Lengths:** PG array elements aren't NUL-terminated; parsers take an explicit length (`parse_long(raw, length)`).