# pgsql fast "raw row -> php object" mapper

> [!WARNING]
> **This extension was written by AI (Claude) and is NOT production-ready.**
> Use at your own risk. The author takes no liability for any damages, data loss,
> crashes, or other issues arising from its use. No guarantees are made regarding
> correctness, stability, or security.

Claude created php extension to map postgresql rows to php objects right in
the C language, with caveats of course, to make it faster.

This makes it **~1.4–1.6x faster** than doing it manually in PHP, or using something like `PDO::FETCH_CLASS`,
which maps only strings, and is also slow, so it's terrible.

### Acknowledgment

From the limited amount of manual testing, extension seems to be somewhat stable, not to crash
php with segfault or other weird errors. If you name columns wrong, it will skip them,
if you set property type of string but return integer from database, it will be casted to string,
and vice versa. It does however breaks quite a lot of PHP assumptions (inheritance for example).
**I have not tested PHP hooks**, because i don't use them, but my guess is that they are not supported.

### API

New function provided by this extension:

```php
\pg_fast_query(\PgSql\Result $result, string $class): array
```

There are two ways to define your classes (DTOs).

**Optimal way is to NOT extend any other DTO and to NOT use constructor**, see examples bellow. 

```php
// you can nest objects arbitrarily :)
readonly class TestNested {
    public int $price;
    public ?\DateTimeImmutable $createdAt = null;
}

// supported, but not recommended, do not use constructor,
// if not required, its way slower, see results at the end
readonly class AccountDto {
    public function __construct(
        public int $account_id,
        public string $login,
        public string $first_name,
        public string $last_name,
        public ?int $frequent_flyer_id = null,
        public ?\DateTimeImmutable $update_ts = null,
    ) {}
}

// supported, but not recommended, do not use constructor,
// if not required, its way slower, see results at the end
readonly class AccountDtoTwo extends AccountDto
{
    public function __construct(
        int $account_id,
        int $login2,
        string $first_name,
        string $last_name,
        ?int $frequent_flyer_id = null,
        ?\DateTimeImmutable $update_ts = null,
        public TestNested $nested,
    ) {
        parent::__construct($account_id, (string)$login2, $first_name, $last_name, $frequent_flyer_id, $update_ts);
    }
}

// recommended way, no contructor, no inheritance, still be able to use object nesting
// properties can be private, and you just create a getter, setters are not needed
readonly class FlatDto {
    public int $account_id;
    public string $login;
    public string $first_name;
    public string $last_name;
    public ?int $frequent_flyer_id;
    public ?\DateTimeImmutable $update_ts;
    public TestNested $nested,
}
```

To use nested objects, you need to name your columns with dot path, eg. from above
```sql
SELECT account_id, price as "nested.price", created_at as "nested.createdAt" FROM your_table;
```
If you have single value objects for example like bellow

```php
readonly class Email {
    private ?string $email;
    
    public function isValid() {
        return $this->email !== null;    
    }
}
```

You don't need to use the dot path to set "nested.email" value, but you can just name is as "nested",
the C extension auto picks first property in class and sets its value to that.

### Build
- `bash build-install.sh`

### Last results

Measured with a fair benchmark: PHP baselines include `pg_fetch_assoc` per row inside the
timing loop, matching the data-access cost the C extension pays reading from `PGresult`.

| Path                                         | C (ns/row) | PHP (ns/row) | Speedup |
|----------------------------------------------|----------:|-------------:|--------:|
| Raw `pg_fetch_assoc` loop (no objects)       |         — |          132 |       — |
| FlatDto — no constructor (fast path)         |       324 |          526 |  **1.63x** |
| AccountDto — promoted-only ctor (fast path)  |       504 |          732 |  **1.45x** |
| AccountDtoTwo — complex ctor + nested object |      1013 |         1386 |  **1.37x** |

The fast path (no constructor, no inheritance) gives the best speedup because the C extension
writes directly to property slots without going through the PHP VM. Once a constructor is
involved, the VM must execute per row and the advantage shrinks to ~1.4x.

**On a real project**, test results are promising. With this extension, mapping to an arbitrary object
with no constructor (the most optimal way) results in basically the same speed as returning plain array,
which is incredible to see. We get free strictly typed and mapped object right from the database with
no additional processing cost, and with lower peak memory usage.

| Endpoint | Req/sec | Avg Lat | p99 Lat | Peak Mem | vs raw-pg |
| :--- | :--- | :--- | :--- | :--- | :--- |
| raw-pg | 4212.39 | 2.89ms | 4.58ms | 20.00 MB | baseline |
| raw-dto | 3331.32 | 3.64ms | 5.61ms | 14.00 MB | 1.26x slower |
| pg-fast | 4271.92 | 2.85ms | 5.04ms | 18.00 MB | 1.01x faster |
| pg-fast-inh | 3714.96 | 3.27ms | 4.87ms | 12.00 MB | 1.13x slower |
| cycle | 795.25 | 15.20ms | 22.30ms | 26.00 MB | 5.29x slower |
| doctrine | 892.82 | 13.58ms | 19.83ms | 18.00 MB | 4.71x slower |

Baseline: raw-pg | wrk: -t4 -c12 -d20s --latency

- **raw-pg** - pg_query_params() + pg_fetch_all(); rows returned as raw associative arrays — no mapping.
- **raw-dto** - pg_query_params() + pg_fetch_assoc() in a while loop; each row immediately mapped into an object with constructor in php.
- **pg-fast** - pg_query() + pg_fast_query() userland helper; deserializes rows directly into an object via this extension.
- **pg-fast-inh** - Same as pg-fast but targets object extending another object (child class), both with constructors; tests overhead of inheritance in the deserialization path.
- **cycle** - Cycle ORM Select + fetchAll(); returns fully hydrated CycleAccount entities with identity map.
- **doctrine** - Doctrine EntityManager findBy(); returns fully hydrated Account entities with lazy-loading and identity map.