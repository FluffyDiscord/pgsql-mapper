# pgsql fast "raw row -> php object" mapper

> [!WARNING]
> **This extension was written by AI (Claude) and is NOT production-ready.**
> Use at your own risk. The author takes no liability for any damages, data loss,
> crashes, or other issues arising from its use. No guarantees are made regarding
> correctness, stability, or security.

Claude created php extension to map postgresql rows to php objects right in
the C language, with caveats of course, to make it faster.

This makes it more than **3 times faster** than doing it manually in PHP, or using something like `PDO::FETCH_CLASS`,
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

| Path                         | Time (ns/row) |
|------------------------------|--------------:|
| Fast path (FlatDto)          |           221 |
| Trivial ctor (AccountDto)    |           316 |
| Complex ctor (AccountDtoTwo) |           576 |
| Manual PHP                   |           749 |
| **Speed up**                 |     **3.39x** |
