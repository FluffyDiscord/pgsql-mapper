<?php
declare(strict_types=1);

class TestNested {
    public int $yep;
    public ?\DateTimeImmutable $ts = null;
}

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

class FlatDto {
    public int $account_id;
    public string $login;
    public string $first_name;
    public string $last_name;
    public ?int $frequent_flyer_id;
    public ?\DateTimeImmutable $update_ts;
}

$conn = pg_connect("host=127.0.0.1 port=15432 dbname=postgres_air user=app password=app");
if (!$conn) {
    die("Connection failed\n");
}

$N = 5000;

pg_query($conn, "DROP TABLE IF EXISTS _perf_test");
pg_query($conn, "
    CREATE TEMP TABLE _perf_test (
        account_id        integer NOT NULL,
        login             text NOT NULL,
        login2            integer NOT NULL,
        first_name        text NOT NULL,
        last_name         text NOT NULL,
        frequent_flyer_id integer,
        update_ts         timestamptz
    )
");

// Bulk insert
$values = [];
for ($i = 1; $i <= $N; $i++) {
    $ffid = ($i % 3 === 0) ? 'NULL' : $i * 10;
    $ts = ($i % 4 === 0) ? 'NULL' : "'2025-06-15 10:30:00+00'";
    $values[] = "($i, 'user_$i', $i, 'First_$i', 'Last_$i', $ffid, $ts)";
}
pg_query($conn, "INSERT INTO _perf_test VALUES " . implode(',', $values));

echo "=== Performance Benchmark ($N rows) ===\n\n";

// --- Benchmark 1: Fast path (FlatDto, no constructor) ---
$result = pg_query($conn, "SELECT account_id, login, first_name, last_name, frequent_flyer_id, update_ts FROM _perf_test ORDER BY account_id");

// Warmup
pg_fast_query($result, FlatDto::class);

$iters = 10;
$best = PHP_INT_MAX;
for ($run = 0; $run < $iters; $run++) {
    $t0 = hrtime(true);
    $rows = pg_fast_query($result, FlatDto::class);
    $elapsed = hrtime(true) - $t0;
    $best = min($best, $elapsed);
}
$ns_fast = $best / $N;
echo sprintf("Fast path (FlatDto, no ctor):    %8.1f ns/row  (best of %d)\n", $ns_fast, $iters);

// --- Benchmark 1b: Trivial ctor (AccountDto, promoted-only — should match fast path) ---
$result1b = pg_query($conn, "SELECT account_id, login, first_name, last_name, frequent_flyer_id, update_ts FROM _perf_test ORDER BY account_id");

// Warmup
pg_fast_query($result1b, AccountDto::class);

$best = PHP_INT_MAX;
for ($run = 0; $run < $iters; $run++) {
    $t0 = hrtime(true);
    $rows = pg_fast_query($result1b, AccountDto::class);
    $elapsed = hrtime(true) - $t0;
    $best = min($best, $elapsed);
}
$ns_trivial = $best / $N;
echo sprintf("Trivial ctor (AccountDto):       %8.1f ns/row  (best of %d)\n", $ns_trivial, $iters);

// --- Benchmark 2: Constructor path (AccountDtoTwo) ---
$result2 = pg_query($conn, 'SELECT account_id, account_id as "nested.yep", login2, first_name, last_name, frequent_flyer_id, update_ts, update_ts as "nested.ts" FROM _perf_test ORDER BY account_id');

// Warmup
pg_fast_query($result2, AccountDtoTwo::class);

$best = PHP_INT_MAX;
for ($run = 0; $run < $iters; $run++) {
    $t0 = hrtime(true);
    $rows = pg_fast_query($result2, AccountDtoTwo::class);
    $elapsed = hrtime(true) - $t0;
    $best = min($best, $elapsed);
}
$ns_ctor = $best / $N;
echo sprintf("Ctor path (AccountDtoTwo):       %8.1f ns/row  (best of %d)\n", $ns_ctor, $iters);

// --- Benchmark 3: Manual PHP mapping ---
$result3 = pg_query($conn, 'SELECT account_id, account_id as "nested_yep", login2, first_name, last_name, frequent_flyer_id, update_ts, update_ts as "nested_ts" FROM _perf_test ORDER BY account_id');
$all = pg_fetch_all($result3);

$best = PHP_INT_MAX;
for ($run = 0; $run < $iters; $run++) {
    $t0 = hrtime(true);
    $mapped = [];
    foreach ($all as $row) {
        $nested = new TestNested();
        $nested->yep = (int)$row['nested_yep'];
        $nested->ts = $row['nested_ts'] !== null ? new \DateTimeImmutable($row['nested_ts']) : null;
        $mapped[] = new AccountDtoTwo(
            account_id: (int)$row['account_id'],
            login2: (int)$row['login2'],
            first_name: $row['first_name'],
            last_name: $row['last_name'],
            frequent_flyer_id: $row['frequent_flyer_id'] !== null ? (int)$row['frequent_flyer_id'] : null,
            update_ts: $row['update_ts'] !== null ? new \DateTimeImmutable($row['update_ts']) : null,
            nested: $nested,
        );
    }
    $elapsed = hrtime(true) - $t0;
    $best = min($best, $elapsed);
}
$ns_php = $best / $N;
echo sprintf("Manual PHP (pg_fetch_all+loop):  %8.1f ns/row  (best of %d)\n", $ns_php, $iters);

echo "\n--- Ratios ---\n";
echo sprintf("Trivial / Fast: %.2fx\n", $ns_trivial / $ns_fast);
echo sprintf("Ctor / Fast:    %.2fx\n", $ns_ctor / $ns_fast);
echo sprintf("PHP  / Fast:    %.2fx\n", $ns_php / $ns_fast);
echo sprintf("PHP  / Ctor:    %.2fx\n", $ns_php / $ns_ctor);

pg_query($conn, "DROP TABLE IF EXISTS _perf_test");
pg_close($conn);
