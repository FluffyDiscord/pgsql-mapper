<?php
declare(strict_types=1);

class FixtureRow {
    public string $raw;
    public array|int|float|bool|null $doc;
}

$data_dir = $argv[1] ?? die("Usage: php test_json_fixtures.php <yyjson>/test/data\n");

$conn = pg_connect("host=127.0.0.1 port=15432 dbname=postgres_air user=app password=app");
if (!$conn) die("Connection failed\n");

pg_query($conn, "DROP CAST IF EXISTS (bytea AS json)");
pg_query($conn, "CREATE CAST (bytea AS json) WITHOUT FUNCTION");

$fixtures = [];
foreach (glob("$data_dir/json/*/*.json") as $file) {
    $fixtures[basename(dirname($file)) . '/' . basename($file)] = file_get_contents($file);
}
foreach (glob("$data_dir/num/*.txt") as $file) {
    foreach (file($file, FILE_IGNORE_NEW_LINES) as $line_number => $line) {
        $is_case = $line !== '' && $line[0] !== '#';
        if ($is_case) {
            $fixtures['num/' . basename($file) . ':' . ($line_number + 1)] = $line;
        }
    }
}

function is_same_outcome(mixed $expected, mixed $actual): bool
{
    $both_threw = $expected instanceof JsonException && $actual instanceof JsonException;
    if ($both_threw) {
        return $expected->getCode() === $actual->getCode();
    }

    $either_threw = $expected instanceof Throwable || $actual instanceof Throwable;
    if ($either_threw) {
        return false;
    }

    return serialize($actual) === serialize($expected);
}

function describe(mixed $outcome): string
{
    if ($outcome instanceof Throwable) {
        return get_class($outcome) . '(' . $outcome->getCode() . ')';
    }

    return substr(serialize($outcome), 0, 120);
}

$passed = 0;
$skipped = [];
$failed = [];

try {
    foreach ($fixtures as $name => $bytes) {
        $result = pg_query_params($conn, 'SELECT doc::text AS raw, doc FROM (SELECT decode($1, \'base64\')::json AS doc) v', [base64_encode($bytes)]);

        $raw = pg_fetch_result($result, 0, 0);
        try {
            $expected = json_decode($raw, true, 512, JSON_THROW_ON_ERROR);
        } catch (JsonException $exception) {
            $expected = $exception;
        }

        if (is_string($expected)) {
            $skipped[] = $name;
            continue;
        }

        try {
            $actual = pg_fast_query($result, FixtureRow::class)[0]->doc;
        } catch (Throwable $exception) {
            $actual = $exception;
        }

        $same = is_same_outcome($expected, $actual);

        if ($same) {
            $passed++;
            continue;
        }

        $failed[] = sprintf("%s\n    expected %s\n    actual   %s", $name, describe($expected), describe($actual));
    }
} finally {
    pg_query($conn, "DROP CAST (bytea AS json)");
    pg_close($conn);
}

foreach ($failed as $failure) {
    echo "FAIL $failure\n";
}
printf("%d fixtures: %d passed, %d failed, %d skipped (root is a JSON string; an array-typed property can't hold it)\n",
    count($fixtures), $passed, count($failed), count($skipped));

exit($failed === [] ? 0 : 1);
