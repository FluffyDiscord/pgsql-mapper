<?php
declare(strict_types=1);

class UserRow {
    public int $id;
    public string $name;
    public float $score;
    public bool $active;
    public DateTimeImmutable $created_at;
}

class ProductRow {
    public int $id;
    public string $sku;
    public ?float $price;
    public ?string $description;
    public ?bool $in_stock;
    public ?DateTimeImmutable $discontinued_at;
}

class EventLog {
    public int $id;
    public string $event_type;
    public ?int $user_id;
    public ?float $latitude;
    public ?float $longitude;
    public ?string $payload;
    public ?DateTimeImmutable $occurred_at;
}

class Address {
    public int $id;
    public string $street;
    public string $city;
}

class OrderInfo {
    public int $id;
    public string $status;
}

class Customer {
    public int $id;
    public string $name;
    public Address $address;
    public OrderInfo $order;
}

class Coordinates {
    public float $lat;
    public float $lng;
}

class City {
    public string $name;
    public Coordinates $coords;
}

class Region {
    public string $name;
    public City $capital;
}

class AutoNestTarget {
    public int $id;
    public Address $location;
}

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

$conn = pg_connect("host=127.0.0.1 port=15432 dbname=postgres_air user=app password=app");
if (!$conn) {
    die("Connection failed\n");
}

/* ===== Test 1: UserRow (all non-null) — PgSql\Result API ===== */
echo "=== Test 1: UserRow (all non-null) ===\n";

pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_users");
pg_query($conn, "
    CREATE TEMP TABLE _mapper_test_users (
        id         serial PRIMARY KEY,
        name       text NOT NULL,
        score      double precision NOT NULL,
        active     boolean NOT NULL,
        created_at timestamptz NOT NULL DEFAULT now()
    )
");
pg_query($conn, "
    INSERT INTO _mapper_test_users (name, score, active, created_at) VALUES
    ('Alice', 99.5, true,  '2025-06-15 10:30:00+00'),
    ('Bob',   42.0, false, '2025-07-20 14:00:00+00'),
    ('Carol', 77.3, true,  '2025-08-01 08:15:30+00')
");

$result = pg_query($conn, "SELECT id, name, score, active, created_at FROM _mapper_test_users ORDER BY id");
$rows = pg_fast_query($result, UserRow::class);

echo "Returned " . count($rows) . " rows\n\n";
foreach ($rows as $i => $row) {
    echo "--- Row $i ---\n";
    echo "  id:         " . var_export($row->id, true) . " (" . get_debug_type($row->id) . ")\n";
    echo "  name:       " . var_export($row->name, true) . " (" . get_debug_type($row->name) . ")\n";
    echo "  score:      " . var_export($row->score, true) . " (" . get_debug_type($row->score) . ")\n";
    echo "  active:     " . var_export($row->active, true) . " (" . get_debug_type($row->active) . ")\n";
    echo "  created_at: " . ($row->created_at instanceof DateTimeImmutable ? $row->created_at->format('Y-m-d H:i:sP') : 'NOT a DateTimeImmutable') . " (" . get_debug_type($row->created_at) . ")\n";
}

assert($rows[0] instanceof UserRow);
assert(is_int($rows[0]->id));
assert(is_string($rows[0]->name));
assert(is_float($rows[0]->score));
assert(is_bool($rows[0]->active));
assert($rows[0]->created_at instanceof DateTimeImmutable);
echo "\nUserRow assertions passed.\n\n";

/* ===== Test 2: ProductRow (nullable columns, mix of null and non-null) ===== */
echo "=== Test 2: ProductRow (nullable properties) ===\n";

pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_products");
pg_query($conn, "
    CREATE TEMP TABLE _mapper_test_products (
        id              serial PRIMARY KEY,
        sku             text NOT NULL,
        price           double precision,
        description     text,
        in_stock        boolean,
        discontinued_at timestamptz
    )
");
pg_query($conn, "
    INSERT INTO _mapper_test_products (sku, price, description, in_stock, discontinued_at) VALUES
    ('WIDGET-01', 19.99, 'A fine widget',    true,  NULL),
    ('GADGET-02', NULL,  NULL,               NULL,  '2025-01-15 00:00:00+00'),
    ('THING-03',  0.0,   '',                 false, '2025-03-20 12:00:00+00')
");

$result = pg_query($conn, "SELECT id, sku, price, description, in_stock, discontinued_at FROM _mapper_test_products ORDER BY id");
$products = pg_fast_query($result, ProductRow::class);

echo "Returned " . count($products) . " rows\n\n";
foreach ($products as $i => $p) {
    echo "--- Product $i ---\n";
    echo "  id:              " . var_export($p->id, true) . " (" . get_debug_type($p->id) . ")\n";
    echo "  sku:             " . var_export($p->sku, true) . " (" . get_debug_type($p->sku) . ")\n";
    echo "  price:           " . var_export($p->price, true) . " (" . get_debug_type($p->price) . ")\n";
    echo "  description:     " . var_export($p->description, true) . " (" . get_debug_type($p->description) . ")\n";
    echo "  in_stock:        " . var_export($p->in_stock, true) . " (" . get_debug_type($p->in_stock) . ")\n";
    echo "  discontinued_at: " . ($p->discontinued_at instanceof DateTimeImmutable ? $p->discontinued_at->format('Y-m-d H:i:sP') : var_export($p->discontinued_at, true)) . " (" . get_debug_type($p->discontinued_at) . ")\n";
}

// Product 0: has values, null discontinued_at
assert($products[0] instanceof ProductRow);
assert($products[0]->price === 19.99);
assert($products[0]->description === 'A fine widget');
assert($products[0]->in_stock === true);
assert($products[0]->discontinued_at === null);

// Product 1: mostly nulls, has discontinued_at
assert($products[1]->price === null);
assert($products[1]->description === null);
assert($products[1]->in_stock === null);
assert($products[1]->discontinued_at instanceof DateTimeImmutable);

// Product 2: zero/empty values (not null!)
assert($products[2]->price === 0.0);
assert($products[2]->description === '');
assert($products[2]->in_stock === false);
assert($products[2]->discontinued_at instanceof DateTimeImmutable);

echo "\nProductRow assertions passed.\n\n";

/* ===== Test 3: EventLog (nullable int, multiple nullable floats, nullable string) ===== */
echo "=== Test 3: EventLog (nullable properties) ===\n";

pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_events");
pg_query($conn, "
    CREATE TEMP TABLE _mapper_test_events (
        id          serial PRIMARY KEY,
        event_type  text NOT NULL,
        user_id     integer,
        latitude    double precision,
        longitude   double precision,
        payload     text,
        occurred_at timestamptz
    )
");
pg_query($conn, "
    INSERT INTO _mapper_test_events (event_type, user_id, latitude, longitude, payload, occurred_at) VALUES
    ('login',    42,   51.5074, -0.1278, '{\"ip\":\"1.2.3.4\"}', '2025-09-01 08:00:00+00'),
    ('anonymous', NULL, NULL,   NULL,    NULL,                    NULL),
    ('geolocate', 7,   NULL,    NULL,    'partial',              '2025-09-02 12:30:00+00')
");

$result = pg_query($conn, "SELECT id, event_type, user_id, latitude, longitude, payload, occurred_at FROM _mapper_test_events ORDER BY id");
$events = pg_fast_query($result, EventLog::class);

echo "Returned " . count($events) . " rows\n\n";
foreach ($events as $i => $e) {
    echo "--- Event $i ---\n";
    echo "  id:          " . var_export($e->id, true) . " (" . get_debug_type($e->id) . ")\n";
    echo "  event_type:  " . var_export($e->event_type, true) . " (" . get_debug_type($e->event_type) . ")\n";
    echo "  user_id:     " . var_export($e->user_id, true) . " (" . get_debug_type($e->user_id) . ")\n";
    echo "  latitude:    " . var_export($e->latitude, true) . " (" . get_debug_type($e->latitude) . ")\n";
    echo "  longitude:   " . var_export($e->longitude, true) . " (" . get_debug_type($e->longitude) . ")\n";
    echo "  payload:     " . var_export($e->payload, true) . " (" . get_debug_type($e->payload) . ")\n";
    echo "  occurred_at: " . ($e->occurred_at instanceof DateTimeImmutable ? $e->occurred_at->format('Y-m-d H:i:sP') : var_export($e->occurred_at, true)) . " (" . get_debug_type($e->occurred_at) . ")\n";
}

// Event 0: all populated
assert($events[0]->user_id === 42);
assert(is_float($events[0]->latitude));
assert(is_float($events[0]->longitude));
assert(is_string($events[0]->payload));
assert($events[0]->occurred_at instanceof DateTimeImmutable);

// Event 1: all nullable columns are null
assert($events[1]->user_id === null);
assert($events[1]->latitude === null);
assert($events[1]->longitude === null);
assert($events[1]->payload === null);
assert($events[1]->occurred_at === null);

// Event 2: mixed null/non-null
assert($events[2]->user_id === 7);
assert($events[2]->latitude === null);
assert($events[2]->longitude === null);
assert($events[2]->payload === 'partial');
assert($events[2]->occurred_at instanceof DateTimeImmutable);

echo "\nEventLog assertions passed.\n\n";

/* ===== Test 4: Nested objects via dot-delimited column names ===== */
echo "=== Test 4: Nested objects (dot notation) ===\n";

pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_nested");
pg_query($conn, "
    CREATE TEMP TABLE _mapper_test_nested (
        id          serial PRIMARY KEY,
        name        text NOT NULL,
        addr_id     integer NOT NULL,
        addr_street text NOT NULL,
        addr_city   text NOT NULL,
        order_id    integer NOT NULL,
        order_status text NOT NULL
    )
");
pg_query($conn, "
    INSERT INTO _mapper_test_nested (name, addr_id, addr_street, addr_city, order_id, order_status) VALUES
    ('Alice', 10, '123 Main St', 'Springfield', 100, 'shipped'),
    ('Bob',   20, '456 Oak Ave', 'Shelbyville', 200, 'pending')
");

$result = pg_query($conn, '
    SELECT
        id,
        name,
        addr_id     AS "address.id",
        addr_street AS "address.street",
        addr_city   AS "address.city",
        order_id    AS "order.id",
        order_status AS "order.status"
    FROM _mapper_test_nested ORDER BY id
');
$customers = pg_fast_query($result, Customer::class);

echo "Returned " . count($customers) . " rows\n\n";
foreach ($customers as $i => $c) {
    echo "--- Customer $i ---\n";
    echo "  id:             " . var_export($c->id, true) . "\n";
    echo "  name:           " . var_export($c->name, true) . "\n";
    echo "  address.id:     " . var_export($c->address->id, true) . "\n";
    echo "  address.street: " . var_export($c->address->street, true) . "\n";
    echo "  address.city:   " . var_export($c->address->city, true) . "\n";
    echo "  order.id:       " . var_export($c->order->id, true) . "\n";
    echo "  order.status:   " . var_export($c->order->status, true) . "\n";
}

// Customer 0
assert($customers[0]->id === 1);
assert($customers[0]->name === 'Alice');
assert($customers[0]->address instanceof Address);
assert($customers[0]->address->id === 10);
assert($customers[0]->address->street === '123 Main St');
assert($customers[0]->address->city === 'Springfield');
assert($customers[0]->order instanceof OrderInfo);
assert($customers[0]->order->id === 100);
assert($customers[0]->order->status === 'shipped');

// Customer 1
assert($customers[1]->id === 2);
assert($customers[1]->name === 'Bob');
assert($customers[1]->address instanceof Address);
assert($customers[1]->address->id === 20);
assert($customers[1]->address->street === '456 Oak Ave');
assert($customers[1]->address->city === 'Shelbyville');
assert($customers[1]->order instanceof OrderInfo);
assert($customers[1]->order->id === 200);
assert($customers[1]->order->status === 'pending');

echo "\nNested object assertions passed.\n\n";

/* ===== Test 5: Deep nesting (3 levels) via dot notation ===== */
echo "=== Test 5: Deep nesting (3 levels) ===\n";

pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_deep");
pg_query($conn, "
    CREATE TEMP TABLE _mapper_test_deep (
        name         text NOT NULL,
        cap_name     text NOT NULL,
        cap_lat      double precision NOT NULL,
        cap_lng      double precision NOT NULL
    )
");
pg_query($conn, "
    INSERT INTO _mapper_test_deep (name, cap_name, cap_lat, cap_lng) VALUES
    ('Midwest',   'Chicago',   41.8781, -87.6298),
    ('Northeast', 'New York',  40.7128, -74.0060)
");

$result = pg_query($conn, '
    SELECT
        name,
        cap_name AS "capital.name",
        cap_lat  AS "capital.coords.lat",
        cap_lng  AS "capital.coords.lng"
    FROM _mapper_test_deep ORDER BY name
');
$regions = pg_fast_query($result, Region::class);

echo "Returned " . count($regions) . " rows\n\n";
foreach ($regions as $i => $r) {
    echo "--- Region $i ---\n";
    echo "  name:               " . var_export($r->name, true) . "\n";
    echo "  capital.name:       " . var_export($r->capital->name, true) . "\n";
    echo "  capital.coords.lat: " . var_export($r->capital->coords->lat, true) . "\n";
    echo "  capital.coords.lng: " . var_export($r->capital->coords->lng, true) . "\n";
}

// Midwest
assert($regions[0]->name === 'Midwest');
assert($regions[0]->capital instanceof City);
assert($regions[0]->capital->name === 'Chicago');
assert($regions[0]->capital->coords instanceof Coordinates);
assert(abs($regions[0]->capital->coords->lat - 41.8781) < 0.001);
assert(abs($regions[0]->capital->coords->lng - (-87.6298)) < 0.001);

// Northeast
assert($regions[1]->name === 'Northeast');
assert($regions[1]->capital->name === 'New York');
assert(abs($regions[1]->capital->coords->lat - 40.7128) < 0.001);
assert(abs($regions[1]->capital->coords->lng - (-74.0060)) < 0.001);

echo "\nDeep nesting assertions passed.\n\n";

/* ===== Test 6: Auto-nest (flat column name, class-typed property) ===== */
echo "=== Test 6: Auto-nest (flat column → class property) ===\n";

pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_autonest");
pg_query($conn, "
    CREATE TEMP TABLE _mapper_test_autonest (
        id       serial PRIMARY KEY,
        location integer NOT NULL
    )
");
pg_query($conn, "
    INSERT INTO _mapper_test_autonest (location) VALUES (42), (99)
");

$result = pg_query($conn, "SELECT id, location FROM _mapper_test_autonest ORDER BY id");
$items = pg_fast_query($result, AutoNestTarget::class);

echo "Returned " . count($items) . " rows\n\n";
foreach ($items as $i => $item) {
    echo "--- Item $i ---\n";
    echo "  id:                 " . var_export($item->id, true) . "\n";
    echo "  location (class):   " . get_debug_type($item->location) . "\n";
    echo "  location.id:        " . var_export($item->location->id, true) . "\n";
}

// Auto-nest: integer value goes to Address->id (first property)
assert($items[0]->id === 1);
assert($items[0]->location instanceof Address);
assert($items[0]->location->id === 42);

assert($items[1]->id === 2);
assert($items[1]->location instanceof Address);
assert($items[1]->location->id === 99);

echo "\nAuto-nest assertions passed.\n\n";

/* ===== Test 7: Constructor with parent class (readonly, promoted props) ===== */
echo "=== Test 7: Constructor with parent::__construct ===\n";

pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_ctor");
pg_query($conn, "
    CREATE TEMP TABLE _mapper_test_ctor (
        account_id        integer NOT NULL,
        login            integer NOT NULL,
        first_name        text NOT NULL,
        last_name         text NOT NULL,
        frequent_flyer_id integer,
        update_ts         timestamptz
    )
");
pg_query($conn, "
    INSERT INTO _mapper_test_ctor VALUES
    (1, 100, 'Alice', 'Smith', 42, '2025-06-15 10:30:00+00'),
    (2, 200, 'Bob', 'Jones', NULL, NULL)
");

$result = pg_query($conn, 'SELECT account_id, account_id as "nested.yep", login as login2, first_name, last_name, frequent_flyer_id, update_ts, update_ts as "nested.ts" FROM _mapper_test_ctor ORDER BY account_id');
$accounts = pg_fast_query($result, AccountDtoTwo::class);

echo "Returned " . count($accounts) . " rows\n\n";
foreach ($accounts as $i => $a) {
    echo "--- Account $i ---\n";
    echo "  account_id:        " . var_export($a->account_id, true) . "\n";
    echo "  login:             " . var_export($a->login, true) . "\n";
    echo "  first_name:        " . var_export($a->first_name, true) . "\n";
    echo "  last_name:         " . var_export($a->last_name, true) . "\n";
    echo "  frequent_flyer_id: " . var_export($a->frequent_flyer_id, true) . "\n";
    echo "  update_ts:         " . ($a->update_ts instanceof DateTimeImmutable ? $a->update_ts->format('Y-m-d H:i:sP') : var_export($a->update_ts, true)) . "\n";
    echo "  nested (class):    " . get_debug_type($a->nested) . "\n";
    echo "  nested.yep:      " . var_export($a->nested->yep, true) . "\n";
    echo "  nested.ts:      " . var_export($a->nested->ts, true) . "\n";
}

// Account 0: all fields populated
assert($accounts[0] instanceof AccountDtoTwo);
assert($accounts[0] instanceof AccountDto);
assert($accounts[0]->account_id === 1);
assert($accounts[0]->login === '100');  // cast to string in child ctor
assert($accounts[0]->first_name === 'Alice');
assert($accounts[0]->last_name === 'Smith');
assert($accounts[0]->frequent_flyer_id === 42);
assert($accounts[0]->update_ts instanceof DateTimeImmutable);
assert($accounts[0]->nested instanceof TestNested);

// Account 1: nullable fields are null
assert($accounts[1]->account_id === 2);
assert($accounts[1]->login === '200');
assert($accounts[1]->first_name === 'Bob');
assert($accounts[1]->last_name === 'Jones');
assert($accounts[1]->frequent_flyer_id === null);
assert($accounts[1]->update_ts === null);
assert($accounts[1]->nested instanceof TestNested);

echo "\nConstructor with parent assertions passed.\n\n";

echo "=== ALL TESTS PASSED ===\n";

pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_users");
pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_products");
pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_events");
pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_nested");
pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_deep");
pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_autonest");
pg_query($conn, "DROP TABLE IF EXISTS _mapper_test_ctor");
pg_close($conn);
