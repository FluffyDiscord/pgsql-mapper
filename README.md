# pgsql-mapper

PHP extension that maps PostgreSQL result rows straight into typed PHP objects. PHP 8.5+, `ext/pgsql` results.

> [!WARNING]
> **This extension was written by AI (Claude) and is NOT production-ready.**
> Use at your own risk. The author takes no liability for any damages, data loss,
> crashes, or other issues arising from its use. No guarantees are made regarding
> correctness, stability, or security.

**Features**

- [Typed values](#type-mapping) — `int`, `float`, `bool`, `DateTimeImmutable` and backed enums, not strings
- [Nested objects](#nested-objects) — `"address.city"` columns fill `$row->address->city`
- [Constructors](#constructors) — promoted-only constructors are skipped: 97 ns/row vs 84 ns/row for a plain DTO
- [Speed](#benchmark) — 84 ns/row vs 515 ns/row for the same mapping in PHP

## Install

```bash
bash build-install.sh
```

Needs `gcc`, `make`, PHP dev headers, libpq headers and `ext/pgsql`. The script names any missing package for your distro.

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

**Columns match properties by name.** Public, protected and private all work. Same speed for each.

## Type mapping

| PostgreSQL | PHP |
|---|---|
| `int2`, `int4`, `int8` | `int` |
| `float4`, `float8` | `float` (`NaN`, `Infinity` → `NAN`, `INF`) |
| `bool` | `bool` |
| `timestamp`, `timestamptz` | `DateTimeImmutable` |
| `NULL` | `null` |
| anything else (`numeric`, `text`, `date`, `json`, `uuid`, arrays…) | `string` |

**Value doesn't fit the property → normal PHP property assignment (coercive mode):**

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

- The subclass constructor isn't called, same as Carbon's own `modify()` etc.
- Subclass with its own required typed property and no default → that property stays uninitialized. Don't use one here.
- Internal subclass with a different object layout → plain `DateTimeImmutable`.

**Unparsed values → `null`** (`TypeError` on a non-nullable property): `infinity`, `-infinity`, BC `timestamp` without zone, and any timestamp in a `DateStyle` other than ISO.

> `timestamptz` in ISO `DateStyle` (the default) skips PHP's generic date parser: 62 ns vs 233 ns per value. `timestamp` without zone and other styles use the generic parser.

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

- Up to 8 levels.
- Nested classes are created without calling their constructor.

**Single-value objects need no dot path.** A column named after a class-typed property fills that class's first declared property, any visibility:

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
| No constructor | Slots written directly | FlatDto 84 ns/row |
| Only promoted params, every param has a column | Constructor skipped, slots written directly | AccountDto 97 ns/row |
| Any other constructor | Called per row; columns matched to params by name, missing params use their defaults | AccountDtoTwo 356 ns/row |
| `__set` or a custom object handler | Every column goes through a normal property write | — |

> Constructor args get raw values: enum and `DateTimeImmutable` subclass mapping don't apply. Columns the constructor doesn't take are written after it runs.
>
> Dot columns for a constructor param's object (`"nested.x"`) are written only when the type already fits. `'42'` into `int` is dropped, not cast.

Not covered:

- Nesting, enum or subclass mapping through union or intersection types
- Hooked properties
- Nesting below a class with `__set` or a custom object handler

## Benchmark

`test_performance.php`, 5000 rows, best of 10 runs, PHP 8.5.12, opcache on. PHP baselines call `pg_fetch_assoc` per row inside the timed loop.

| Path | C (ns/row) | PHP (ns/row) | Speedup |
|---|---:|---:|---:|
| `pg_fetch_assoc` loop, no objects | — | 118 | — |
| FlatDto, no constructor | 84 | 515 | **6.1x** |
| AccountDto, promoted-only constructor | 97 | 580 | **6.0x** |
| AccountDtoTwo, custom constructor + nested object | 356 | 1064 | **3.0x** |

Mapping into a plain DTO is faster than `pg_fetch_assoc` returning arrays.

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
