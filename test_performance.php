<?php
declare(strict_types=1);

/*
 * Fair benchmark: each PHP baseline includes pg_fetch_assoc per row inside
 * the timing loop — the same cost the C extension pays reading from PGresult.
 * No pre-fetching outside the loop.
 *
 * C results reuse the same PgSql\Result (PQgetvalue is random-access on the
 * in-memory buffer, pg_fast_query never advances rh->row).
 * PHP results use pg_result_seek($r, 0) to reset the row cursor each run.
 */

class TestNested {
    public int $yep;
    public ?\DateTimeImmutable $ts = null;
}

class FlatDto {
    public int $account_id;
    public string $login;
    public string $first_name;
    public string $last_name;
    public ?int $frequent_flyer_id;
    public ?\DateTimeImmutable $update_ts;
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

$conn = pg_connect("host=127.0.0.1 port=15432 dbname=postgres_air user=app password=app");
if (!$conn) die("Connection failed\n");

$N     = 5000;
$iters = 10;

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
$values = [];
for ($i = 1; $i <= $N; $i++) {
    $ffid = ($i % 3 === 0) ? 'NULL' : $i * 10;
    $ts   = ($i % 4 === 0) ? 'NULL' : "'2025-06-15 10:30:00+00'";
    $values[] = "($i, 'user_$i', $i, 'First_$i', 'Last_$i', $ffid, $ts)";
}
pg_query($conn, "INSERT INTO _perf_test VALUES " . implode(',', $values));

$flat_sql   = "SELECT account_id, login, first_name, last_name, frequent_flyer_id, update_ts FROM _perf_test ORDER BY account_id";
$nested_sql = 'SELECT account_id, account_id AS "nested.yep", login2, first_name, last_name, frequent_flyer_id, update_ts, update_ts AS "nested.ts" FROM _perf_test ORDER BY account_id';
$nested_sql_flat_cols = 'SELECT account_id, account_id AS nested_yep, login2, first_name, last_name, frequent_flyer_id, update_ts, update_ts AS nested_ts FROM _perf_test ORDER BY account_id';

echo "=== Fair Performance Benchmark ($N rows, best of $iters runs) ===\n\n";

// -------------------------------------------------------------------------
// RAW BASELINES (no object creation — pure data-fetch cost)
// -------------------------------------------------------------------------

$r_raw = pg_query($conn, $flat_sql);

// Raw: pg_fetch_all (builds full PHP array-of-arrays)
$best = PHP_INT_MAX;
for ($run = 0; $run < $iters; $run++) {
    pg_result_seek($r_raw, 0);
    $t0 = hrtime(true);
    $all = pg_fetch_all($r_raw);
    $best = min($best, hrtime(true) - $t0);
}
$ns_fetch_all = $best / $N;
echo sprintf("Raw pg_fetch_all:                     %8.1f ns/row\n", $ns_fetch_all);

// Raw: pg_fetch_assoc loop (one array per row, no accumulation)
$best = PHP_INT_MAX;
for ($run = 0; $run < $iters; $run++) {
    pg_result_seek($r_raw, 0);
    $t0 = hrtime(true);
    while (pg_fetch_assoc($r_raw) !== false) {}
    $best = min($best, hrtime(true) - $t0);
}
$ns_fetch_assoc = $best / $N;
echo sprintf("Raw pg_fetch_assoc loop:              %8.1f ns/row\n", $ns_fetch_assoc);

echo "\n";

// -------------------------------------------------------------------------
// FLAT DTO — no constructor, direct property writes
// -------------------------------------------------------------------------

// C extension: fast path (direct slot writes, no VM)
$rc_flat = pg_query($conn, $flat_sql);
pg_fast_query($rc_flat, FlatDto::class); // warmup
$best = PHP_INT_MAX;
for ($run = 0; $run < $iters; $run++) {
    $t0 = hrtime(true);
    $rows = pg_fast_query($rc_flat, FlatDto::class);
    $best = min($best, hrtime(true) - $t0);
}
$ns_c_flat = $best / $N;
echo sprintf("C  FlatDto (fast path):               %8.1f ns/row\n", $ns_c_flat);

// PHP manual: pg_fetch_assoc + new FlatDto + typed assigns
$r_flat = pg_query($conn, $flat_sql);
$best = PHP_INT_MAX;
for ($run = 0; $run < $iters; $run++) {
    pg_result_seek($r_flat, 0);
    $t0 = hrtime(true);
    $rows = [];
    while ($row = pg_fetch_assoc($r_flat)) {
        $o = new FlatDto();
        $o->account_id        = (int)$row['account_id'];
        $o->login             = $row['login'];
        $o->first_name        = $row['first_name'];
        $o->last_name         = $row['last_name'];
        $o->frequent_flyer_id = $row['frequent_flyer_id'] !== null ? (int)$row['frequent_flyer_id'] : null;
        $o->update_ts         = $row['update_ts'] !== null ? new \DateTimeImmutable($row['update_ts']) : null;
        $rows[] = $o;
    }
    $best = min($best, hrtime(true) - $t0);
}
$ns_php_flat = $best / $N;
echo sprintf("PHP FlatDto (fetch_assoc + assign):   %8.1f ns/row   speedup: %.2fx\n",
    $ns_php_flat, $ns_php_flat / $ns_c_flat);

echo "\n";

// -------------------------------------------------------------------------
// AccountDto — trivial promoted-only ctor (C bypasses VM via is_trivial_promoted_ctor)
// -------------------------------------------------------------------------

// C extension: trivial ctor detection → still uses fast-path slot writes
$rc_adt = pg_query($conn, $flat_sql);
pg_fast_query($rc_adt, AccountDto::class); // warmup
$best = PHP_INT_MAX;
for ($run = 0; $run < $iters; $run++) {
    $t0 = hrtime(true);
    $rows = pg_fast_query($rc_adt, AccountDto::class);
    $best = min($best, hrtime(true) - $t0);
}
$ns_c_adt = $best / $N;
echo sprintf("C  AccountDto (trivial ctor → fast):  %8.1f ns/row\n", $ns_c_adt);

// PHP manual: pg_fetch_assoc + new AccountDto(...) via constructor
$r_adt = pg_query($conn, $flat_sql);
$best = PHP_INT_MAX;
for ($run = 0; $run < $iters; $run++) {
    pg_result_seek($r_adt, 0);
    $t0 = hrtime(true);
    $rows = [];
    while ($row = pg_fetch_assoc($r_adt)) {
        $rows[] = new AccountDto(
            account_id:        (int)$row['account_id'],
            login:             $row['login'],
            first_name:        $row['first_name'],
            last_name:         $row['last_name'],
            frequent_flyer_id: $row['frequent_flyer_id'] !== null ? (int)$row['frequent_flyer_id'] : null,
            update_ts:         $row['update_ts'] !== null ? new \DateTimeImmutable($row['update_ts']) : null,
        );
    }
    $best = min($best, hrtime(true) - $t0);
}
$ns_php_adt = $best / $N;
echo sprintf("PHP AccountDto (fetch_assoc + ctor):  %8.1f ns/row   speedup: %.2fx\n",
    $ns_php_adt, $ns_php_adt / $ns_c_adt);

echo "\n";

// -------------------------------------------------------------------------
// AccountDtoTwo — complex ctor + nested object via dot-columns
// -------------------------------------------------------------------------

// C extension: ctor path (VM invoked per row)
$rc_adt2 = pg_query($conn, $nested_sql);
pg_fast_query($rc_adt2, AccountDtoTwo::class); // warmup
$best = PHP_INT_MAX;
for ($run = 0; $run < $iters; $run++) {
    $t0 = hrtime(true);
    $rows = pg_fast_query($rc_adt2, AccountDtoTwo::class);
    $best = min($best, hrtime(true) - $t0);
}
$ns_c_adt2 = $best / $N;
echo sprintf("C  AccountDtoTwo (complex ctor):      %8.1f ns/row\n", $ns_c_adt2);

// PHP manual: pg_fetch_assoc + manual nested + new AccountDtoTwo(...)
$r_adt2 = pg_query($conn, $nested_sql_flat_cols);
$best = PHP_INT_MAX;
for ($run = 0; $run < $iters; $run++) {
    pg_result_seek($r_adt2, 0);
    $t0 = hrtime(true);
    $rows = [];
    while ($row = pg_fetch_assoc($r_adt2)) {
        $nested      = new TestNested();
        $nested->yep = (int)$row['nested_yep'];
        $nested->ts  = $row['nested_ts'] !== null ? new \DateTimeImmutable($row['nested_ts']) : null;
        $rows[] = new AccountDtoTwo(
            account_id:        (int)$row['account_id'],
            login2:            (int)$row['login2'],
            first_name:        $row['first_name'],
            last_name:         $row['last_name'],
            frequent_flyer_id: $row['frequent_flyer_id'] !== null ? (int)$row['frequent_flyer_id'] : null,
            update_ts:         $row['update_ts'] !== null ? new \DateTimeImmutable($row['update_ts']) : null,
            nested:            $nested,
        );
    }
    $best = min($best, hrtime(true) - $t0);
}
$ns_php_adt2 = $best / $N;
echo sprintf("PHP AccountDtoTwo (fetch_assoc+ctor): %8.1f ns/row   speedup: %.2fx\n",
    $ns_php_adt2, $ns_php_adt2 / $ns_c_adt2);

echo "\n--- Summary ---\n";
echo sprintf("Data fetch overhead (pg_fetch_assoc): %8.1f ns/row\n", $ns_fetch_assoc);
echo sprintf("Data fetch overhead (pg_fetch_all):   %8.1f ns/row\n", $ns_fetch_all);

pg_query($conn, "DROP TABLE IF EXISTS _perf_test");
pg_close($conn);
