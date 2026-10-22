# pgsql-mapper

PHP extension that maps PostgreSQL result rows straight into typed PHP objects. PHP 8.5+, `ext/pgsql` results.

**Features**

- [Typed values](#type-mapping) — `int`, `float`, `bool`, `DateTimeImmutable` and backed enums, not strings
- [JSON and arrays](#json-and-arrays) — `jsonb` and `int4[]`/`text[]`… straight into PHP arrays, 3.6–16.0x faster than decoding in PHP
- [Nested objects](#nested-objects) — `"address.city"` columns fill `$row->address->city`
- [Constructors](#constructors) — promoted-only constructors cost 100 ns/row vs 88 ns/row for a plain DTO
- [Speed](#benchmark) — 88 ns/row vs 524 ns/row for the same mapping in PHP

## Install

```bash
bash build-install.sh
```

Needs `gcc`, `make`, PHP dev headers, libpq and [yyjson](https://github.com/ibireme/yyjson) headers, and `ext/pgsql`. The script names any missing package for your distro (`yyjson-devel` / `libyyjson-dev`).

IDE stubs:

```bash
composer require --dev fluffydiscord/fast-pgsql-mapper-stubs
```

## Usage

1. `src/AccountRow.php`

   ```php
   readonly class AccountRow
   {
       public int $id;
       public string $login;
       public ?DateTimeImmutable $updatedAt;
   }
   ```

2. Query and map:

   ```php
   $result = pg_query($connection, 'SELECT id, login, update_ts AS "updatedAt" FROM account');

   /** @var list<AccountRow> $rows */
   $rows = pg_fast_query($result, AccountRow::class);
   ```

**Columns match properties by name.** Public, protected and private all work, at the same speed.

## Type mapping

| PostgreSQL | PHP |
|---|---|
| `int2`, `int4`, `int8` | `int` |
| `float4`, `float8` | `float` (`NaN`, `Infinity` → `NAN`, `INF`) |
| `bool` | `bool` |
| `timestamp`, `timestamptz` | `DateTimeImmutable` |
| `NULL` | `null` |
| anything else (`numeric`, `text`, `uuid`…) | `string` |

**Some types depend on the property type.** Otherwise they stay `string`:

| PostgreSQL | Property type | PHP |
|---|---|---|
| `date` | `DateTimeImmutable` (or subclass, `DateTimeInterface`) | midnight UTC |
| `json`, `jsonb` | `array` | [decoded array](#json-and-arrays) |
| any array (`int4[]`, `text[]`…) | `array` | [PHP list](#json-and-arrays) |
| text column | backed enum | [enum case](#backed-enums) |

A property that also accepts `string` (`string|array`, untyped) gets the raw string.

**Value doesn't fit the property → normal PHP assignment (coercive mode):**

| Case | Result |
|---|---|
| `5` into `string` | `'5'` |
| `'42'` into `int` | `42` |
| `'abc'` into `int` | `TypeError` |
| `NULL` into non-nullable | `TypeError` |
| Column with no matching property | skipped |
| Property with no column | left uninitialized |
| Property with a hook | skipped |

### Datetime

**Type a property as a `DateTimeImmutable` subclass to get that class:**

```php
public ?\Carbon\CarbonImmutable $createdAt; // CarbonImmutable, not DateTimeImmutable
```

- The subclass constructor isn't called.
- Subclass with its own required typed property and no default → that property stays uninitialized. Don't use one here.
- Internal subclass with a different object layout → plain `DateTimeImmutable`.

**`date` → midnight UTC**, whatever `date.timezone` says: `2025-01-02` → `2025-01-02T00:00:00+00:00`.

**Unparsed values → `null`** (`TypeError` on a non-nullable property): `infinity`, `-infinity`, BC `timestamp` without zone, any `DateStyle` other than ISO.

> `timestamptz` in ISO `DateStyle` (the default) skips PHP's generic date parser: 6.8x faster than `new DateTimeImmutable()` ([benchmark](#benchmark)).

### Backed enums

**Type a property as a backed enum to get the case:**

```php
enum Status: string { case Active = 'active'; case Ended = 'ended'; }

readonly class Row
{
    public Status $status;   // 'active' → Status::Active
    public ?Status $maybe;   // NULL → null
}
```

- String-backed ← text columns. Int-backed ← integer columns. Other pairs → raw value.
- Unknown value → `ValueError`, same as `Status::from()`.
- Pure (non-backed) enums aren't mapped.

## JSON and arrays

**Type the property as `array`:**

```php
readonly class Product
{
    public array $attributes;  // jsonb '{"color": "red", "sizes": [1, 2]}' → ['color' => 'red', 'sizes' => [1, 2]]
    public array $tagIds;      // int4[] '{1,2,NULL}' → [1, 2, null]
    public ?array $labels;     // text[] '{"a b",c}' → ['a b', 'c']
}
```

- `json`/`jsonb` → same as `json_decode($value, true, 512, JSON_THROW_ON_ERROR)`. Invalid JSON (e.g. non-UTF-8 `client_encoding`) or deeper than 512 levels → `JsonException`, even on a nullable property.
- JSON scalar (`"text"`, `42`) → `TypeError`.
- Elements convert like columns: `int4[]` → `int`, `float8[]` → `float`, `bool[]` → `bool`, `timestamptz[]` → `DateTimeImmutable`, anything else → `string`.
- Multi-dimensional → nested lists. Custom bounds (`[0:2]={…}`) → 0-based list.
- `text` column holding `'{a,b}'` → `['a', 'b']`.
- Unions like `bool|array` keep scalar columns as they are.

## Nested objects

**Name columns with a dot path:**

```sql
SELECT id, price AS "nested.price", created_at AS "nested.createdAt" FROM orders
```

```php
readonly class Nested
{
    public int $price;
    public ?DateTimeImmutable $createdAt;
}

readonly class Order
{
    public int $id;
    public Nested $nested;
}
```

- Up to 8 levels. Deeper → column ignored.
- Nested objects are created without calling their constructor.
- Interface or abstract-class properties aren't created; the column goes through normal assignment.

**Single-value objects need no dot path.** A column named after a class-typed property fills that class's first declared property:

```php
readonly class Email
{
    private ?string $email;

    public function isValid(): bool
    {
        return $this->email !== null;
    }
}

readonly class User
{
    public Email $email;   // SELECT email FROM users → $user->email->isValid()
}
```

## Constructors

| Class | What happens | [Benchmark](#benchmark) |
|---|---|---|
| No constructor | Slots written directly | FlatDto 88 ns/row |
| Only promoted params, every param has a column | Constructor skipped, slots written directly | AccountDto 100 ns/row |
| Any other constructor | Called per row; columns matched to params by name, missing params use their defaults | AccountDtoTwo 277 ns/row |
| Internal (C) constructor, e.g. extending `Exception` | Called without args; columns written after it | — |
| `__set` or a custom object handler | Every column goes through a normal property write | — |

> Constructor args get raw values: enum, `date`, JSON, array and `DateTimeImmutable` subclass mapping don't apply. Columns the constructor doesn't take are written after it runs.

Not covered:

- Nesting, enum or subclass mapping through union or intersection types
- JSON objects as `stdClass` (always arrays)
- `box[]` (uses `;` between elements) → raw string
- Nesting below a class with `__set` or a custom object handler

## Benchmark

`php test_performance.php`: 5000 rows, best of 10 runs, PHP 8.5.12 CLI without opcache. Every PHP baseline calls `pg_fetch_assoc` per row inside the timed loop.

| DTO | C (ns/row) | PHP (ns/row) | Speedup |
|---|---:|---:|---:|
| `pg_fetch_assoc` loop, no objects | — | 116 | — |
| FlatDto, no constructor | 88 | 524 | **5.9x** |
| AccountDto, promoted-only constructor | 100 | 580 | **5.8x** |
| AccountDtoTwo, custom constructor + nested object | 277 | 987 | **3.6x** |

Mapping into a plain DTO is faster than `pg_fetch_assoc` returning plain arrays.

| Column → property | C (ns/row) | PHP baseline | PHP (ns/row) | Speedup |
|---|---:|---|---:|---:|
| `jsonb` ~80 B → `array` | 117 | `json_decode` | 486 | **4.2x** |
| `jsonb` ~1 KB → `array` | 1315 | `json_decode` | 5101 | **3.9x** |
| 3 `jsonb` (~1.1 KB, chat message) → `array` | 842 | `json_decode` | 3027 | **3.6x** |
| same 3 `jsonb` → `string`, no decoding | 44 | — | 136 | — |
| `int4[]`, 10 items → `array` | 70 | `explode` + `intval` | 332 | **4.8x** |
| `text[]`, 5 quoted items → `array` | 59 | `str_getcsv` | 943 | **16.0x** |
| `timestamptz` → `DateTimeImmutable` | 54 | `new DateTimeImmutable` | 351 | **6.5x** |
| `date` → `DateTimeImmutable` | 51 | `new DateTimeImmutable` | 380 | **7.5x** |

### Real project

`wrk -t4 -c12 -d20s --latency`, baseline raw-pg.

> Measured with v1.1.0 or earlier.

| Endpoint | Req/sec | Avg Lat | p99 Lat | Peak Mem | vs raw-pg |
| :--- | :--- | :--- | :--- | :--- | :--- |
| raw-pg | 4212.39 | 2.89ms | 4.58ms | 20.00 MB | baseline |
| raw-dto | 3331.32 | 3.64ms | 5.61ms | 14.00 MB | 1.26x slower |
| pg-fast | 4271.92 | 2.85ms | 5.04ms | 18.00 MB | 1.01x faster |
| pg-fast-inh | 3714.96 | 3.27ms | 4.87ms | 12.00 MB | 1.13x slower |
| cycle | 795.25 | 15.20ms | 22.30ms | 26.00 MB | 5.29x slower |
| doctrine | 892.82 | 13.58ms | 19.83ms | 18.00 MB | 4.71x slower |

- **raw-pg** — `pg_query_params()` + `pg_fetch_all()`, plain arrays.
- **raw-dto** — `pg_fetch_assoc()` loop, each row mapped through a PHP constructor.
- **pg-fast** — `pg_query()` + `pg_fast_query()`.
- **pg-fast-inh** — same, into a child class; both classes have constructors.
- **cycle** — Cycle ORM `Select` + `fetchAll()`, identity map.
- **doctrine** — Doctrine `findBy()`, lazy loading and identity map.
