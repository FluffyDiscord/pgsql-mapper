# pgsql_mapper Development Guide

## Overview
A PHP C extension that hydrates PostgreSQL results directly into typed PHP objects.
**Signature:** `pg_fast_query(PgSql\Result $result, string $class): array`

## Project & Build
* **Core Logic:** `pgsql_mapper.c` (main extension) and `php_pgsql_mapper.h` (vendored PgSql structs & OIDs).
* **Build:** Run `bash build-install.sh` for a full rebuild/install. (Run `phpize && ./configure` first if `config.m4`/`configure.ac` change).
* **Tests:** Run `php test.php` (Requires local PG at `127.0.0.1:15432`, db=`postgres_air`, user=`app`).

## Architecture
* **Execution Paths:**
    * **Fast:** Direct memory assignment for classes without `__set` or custom object handlers.
    * **Slow:** Uses `zend_update_property_ex()` for classes with magic methods.
* **Object Mapping:**
    * **Dot Notation:** Columns like `"address.city"` map to `$obj->address->city` (max depth: 8). Instances are reused if referenced multiple times.
    * **Auto-nesting:** If a column targets a user class, the extension auto-creates the object and sets the value on its **first declared public property**.
* **Type Conversion:** PostgreSQL OIDs map to `int`, `float`, `bool`, `DateTimeImmutable` (timestamps), and `string` (fallback).
* **Vendored Structs:** The internal structs in `php_pgsql_mapper.h` **must** match the PHP version being compiled against.

## Adding a New Type Mapping
1. Define the OID constant in `php_pgsql_mapper.h`.
2. Add a `case` to the conversion `switch` in `pg_fast_query`.
3. Add the type to `can_skip_type_check()` (for fast path support).
4. Write a test case in `test.php`.

## Critical Pitfalls
* **Lifecycle:** **Do NOT** `PQclear` the result; PHP's ext/pgsql owns it.
* **Memory:** Always run `zval_ptr_dtor` before overwriting property slots to free old values.
* **Properties:** Always check and skip `ZEND_ACC_STATIC`, non-public (`!ZEND_ACC_PUBLIC`), and PHP 8.4+ hooked properties.