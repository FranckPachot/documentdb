# pg_docinspect

A PostgreSQL extension for inspecting how documents are stored in BSON and JSONB formats, and what index entries point to them. Think of it as `pageinspect` but for document-level introspection.

## Quick Start

```bash
# Build (from the pg_docinspect directory)
docker build -t pg-docinspect .

# Run
docker run -d --name lab pg-docinspect --password mypass

# Connect
docker exec -it lab psql -h localhost -p 9712 -U documentdb -d postgres

# Enable the extension
CREATE EXTENSION docinspect;
```

## Functions

| Function | Description |
|----------|-------------|
| `docinspect.bson_inspect(bson)` | SRF: BSON binary layout as rows |
| `docinspect.bson_inspect_pretty(bson)` | Text: indented BSON binary layout |
| `docinspect.jsonb_inspect(jsonb)` | SRF: JSONB binary layout as rows |
| `docinspect.jsonb_inspect_pretty(jsonb)` | Text: indented JSONB binary layout |
| `docinspect.index_entries(regclass, tid)` | SRF: all index entries for a heap tuple |
| `docinspect.collection_table(db, coll)` | Resolves collection name → regclass |
| `docinspect.collection_index_entries(db, coll, id)` | Index entries by document _id |
| `docinspect.collection_index_terms(db, coll, id)` | Human-readable index terms by _id |

---

## Full Examples

### 1. BSON Inspection

BSON stores documents as a sequence of typed elements: `[type_byte][key\0][value]`.

```sql
-- Simple document
SELECT docinspect.bson_inspect_pretty(
  '{"name": "Alice", "age": 30, "scores": [95, 87, 92]}'::documentdb_core.bson
);
```

Output:
```
BSON Document (74 bytes)
========================================
[0000] 0xff DocHeader    "size=74"  (4 bytes)
[0004] 0x02 String       "name"  (17 bytes)  = "Alice"
[0021] 0x10 Int32        "age"  (9 bytes)  = 30
[0030] 0x04 Array        "scores"  (39 bytes)
  [0038] 0xff DocHeader    "size=31"  (4 bytes)
  [0042] 0x10 Int32        "0"  (7 bytes)  = 95
  [0049] 0x10 Int32        "1"  (7 bytes)  = 87
  [0056] 0x10 Int32        "2"  (7 bytes)  = 92
  [0063] 0x00 EOD           (1 bytes)  = 0x00 terminator
[0064] 0x00 EOD           (1 bytes)  = 0x00 terminator
```

```sql
-- Tabular view (useful for filtering)
SELECT depth, type_hex, type_name, field_name, byte_length, value_repr
FROM docinspect.bson_inspect(
  '{"_id": {"$oid": "507f1f77bcf86cd799439011"}, "status": "active"}'::documentdb_core.bson
);
```

```sql
-- Nested document
SELECT docinspect.bson_inspect_pretty(
  '{"user": {"name": "Bob", "address": {"city": "NYC", "zip": "10001"}}}'::documentdb_core.bson
);
```

```sql
-- Document with various BSON types
SELECT docinspect.bson_inspect_pretty(
  '{"_id": {"$oid": "507f1f77bcf86cd799439011"}, "count": {"$numberLong": "9999"}, "active": true, "score": 3.14}'::documentdb_core.bson
);
```

### 2. JSONB Inspection

JSONB stores data as containers (Object/Array) with JEntry arrays encoding type+offset for each element. Keys are sorted alphabetically and deduplicated.

```sql
-- Simple document
SELECT docinspect.jsonb_inspect_pretty(
  '{"name": "Alice", "age": 30, "active": true}'::jsonb
);
```

Output:
```
JSONB Value (52 bytes after varlena header)
========================================
[0000] JSONB Header   (52 bytes)  = 52 bytes total (after varlena header)
[0000] Object         (4 bytes)  = header=0x00000003, 3 keys, 52 bytes
  [0028] Key           (6 bytes)  = "active"
  [0034] Key           (3 bytes)  = "age"
  [0037] Key           (4 bytes)  = "name"
  [0041] Boolean  active  = true
  [0041] Numeric  age  (8 bytes)  = 30
  [0049] String   name  (5 bytes)  = "Alice"
[0054] EndObject
```

Notice how JSONB sorts keys alphabetically (`active`, `age`, `name`) regardless of insertion order.

```sql
-- Compare BSON vs JSONB for the same document
SELECT docinspect.bson_inspect_pretty(
  '{"z_last": 1, "a_first": 2}'::documentdb_core.bson
);
-- BSON preserves insertion order: z_last comes first

SELECT docinspect.jsonb_inspect_pretty(
  '{"z_last": 1, "a_first": 2}'::jsonb
);
-- JSONB sorts keys: a_first comes first
```

```sql
-- Array with mixed types
SELECT docinspect.jsonb_inspect_pretty(
  '{"tags": ["urgent", null, true, 3.14, {"nested": "obj"}]}'::jsonb
);
```

### 3. Index Entry Inspection

Shows what index entries (btree, GIN, RUM) would be generated for a specific heap tuple.

#### Setup: JSONB table with indexes (works standalone)

```sql
-- Create a JSONB table
CREATE TABLE events (
    id serial PRIMARY KEY,
    payload jsonb NOT NULL
);

-- Insert data
INSERT INTO events (payload) VALUES
  ('{"type": "click", "page": "/home", "ts": 1700000000}'),
  ('{"type": "view", "page": "/about", "duration": 5.2}'),
  ('{"type": "click", "page": "/home", "button": "signup"}');

-- Create indexes
CREATE INDEX idx_events_payload ON events USING gin (payload jsonb_ops);
CREATE INDEX idx_events_type ON events ((payload->>'type'));
CREATE INDEX idx_events_page ON events ((payload->>'page'));
```

#### Inspect index entries for a specific row

```sql
-- Find the ctid (physical tuple location) of a row
SELECT ctid, id, payload->>'type' as type FROM events;
--  ctid  | id | type
-- -------+----+-------
--  (0,1) |  1 | click
--  (0,2) |  2 | view
--  (0,3) |  3 | click

-- Show all index entries pointing to the first row
SELECT * FROM docinspect.index_entries('events'::regclass, '(0,1)'::tid);
```

Expected output:
```
   index_name       | index_type |              entry_repr               | entry_bytes
--------------------+------------+-----------------------------------------+-------------
 events_pkey        | btree      | (1)                                     | \x...
 idx_events_payload | gin        | col0 term[1/6]: "click"                 | \x...
 idx_events_payload | gin        | col0 term[2/6]: "/home"                 | \x...
 idx_events_payload | gin        | col0 term[3/6]: 1700000000              | \x...
 idx_events_payload | gin        | col0 term[4/6]: "page"                  | \x...
 idx_events_payload | gin        | col0 term[5/6]: "ts"                    | \x...
 idx_events_payload | gin        | col0 term[6/6]: "type"                  | \x...
 idx_events_type    | btree      | (click)                                 | \x...
 idx_events_page    | btree      | (/home)                                 | \x...
```

#### DocumentDB collection with indexes (via MongoDB API)

DocumentDB creates indexes through its own API. The underlying PostgreSQL tables
use RUM/GIN indexes with a custom `bsonindexterm` type.

```sql
-- Create a collection and insert documents (via DocumentDB API)
SELECT documentdb_api.insert_one('mydb', 'users',
  '{"_id": 1, "name": "Alice", "age": 30, "city": "NYC"}'::documentdb_core.bson);
SELECT documentdb_api.insert_one('mydb', 'users',
  '{"_id": 2, "name": "Bob", "age": 25, "city": "LA"}'::documentdb_core.bson);

-- Create an index on "name" (via DocumentDB API)
SELECT documentdb_api_internal.create_indexes_non_concurrently('mydb',
  '{"createIndexes": "users", "indexes": [{"key": {"name": 1}, "name": "name_1"}]}',
  TRUE);
```

##### Using helper functions (no need to know the table number)

```sql
-- Resolve collection name → underlying table automatically
SELECT docinspect.collection_table('mydb', 'users');
-- Returns: documentdb_data.documents_3

-- Show index terms for a document by _id (human-readable output)
SELECT * FROM docinspect.collection_index_terms(
  'mydb', 'users', '{"": 1}'::documentdb_core.bson
);
--  index_name  |  index_type  |       term_bson
-- -------------+--------------+------------------------------------------
--  _id_        | btree        | (3, { "" : 1 })
--  name_1      | rum          | { "name" : "Alice" }

-- Or get raw index entries with bytes
SELECT * FROM docinspect.collection_index_entries(
  'mydb', 'users', '{"": 1}'::documentdb_core.bson
);
```

##### Manual approach (explore the raw table directly)

```sql
-- Find the underlying table number
SELECT collection_id FROM documentdb_api_catalog.collections
WHERE database_name = 'mydb' AND collection_name = 'users';
-- e.g. collection_id = 3

-- Look at the table structure and indexes
\d documentdb_data.documents_3

-- Find a tuple's ctid
SELECT ctid, object_id, document
FROM documentdb_data.documents_3 LIMIT 2;

-- Inspect all index entries for that tuple
SELECT index_name, index_type, left(entry_repr, 80) as entry
FROM docinspect.index_entries('documentdb_data.documents_3'::regclass, '(0,1)'::tid);
```

This shows you the RUM index terms that DocumentDB generates — including the
serialized `bsonindexterm` values with path prefixes and type metadata.

### 4. Comparing Storage Formats

A useful exercise: store the same logical document in both formats and compare.

```sql
-- Same document in both formats
\x off

SELECT docinspect.bson_inspect_pretty(
  '{"product": "Widget", "price": 9.99, "in_stock": true, "tags": ["sale", "new"]}'::documentdb_core.bson
);

SELECT docinspect.jsonb_inspect_pretty(
  '{"product": "Widget", "price": 9.99, "in_stock": true, "tags": ["sale", "new"]}'::jsonb
);
```

Key differences you'll observe:
- **BSON** preserves field order, uses type byte per element, stores array indices as string keys
- **JSONB** sorts keys alphabetically, uses JEntry arrays with type flags, more compact for queries
- **BSON** has richer types (ObjectId, DateTime, Decimal128, Int32 vs Int64)
- **JSONB** has only: string, numeric, boolean, null, object, array

---

## BSON Type Reference

| Hex  | Code | Name        | Value Size |
|------|------|-------------|------------|
| 0x01 | 1    | Double      | 8 bytes |
| 0x02 | 2    | String      | 4 (len) + string + null |
| 0x03 | 3    | Document    | nested document |
| 0x04 | 4    | Array       | nested document |
| 0x05 | 5    | Binary      | 4 (len) + 1 (subtype) + data |
| 0x06 | 6    | Undefined   | 0 bytes |
| 0x07 | 7    | ObjectId    | 12 bytes |
| 0x08 | 8    | Boolean     | 1 byte |
| 0x09 | 9    | DateTime    | 8 bytes (int64 ms since epoch) |
| 0x0A | 10   | Null        | 0 bytes |
| 0x0B | 11   | Regex       | cstring + cstring |
| 0x0D | 13   | Code        | 4 (len) + string + null |
| 0x0F | 15   | CodeWScope  | 4 + 4 + code + scope_doc |
| 0x10 | 16   | Int32       | 4 bytes |
| 0x11 | 17   | Timestamp   | 8 bytes (uint32 + uint32) |
| 0x12 | 18   | Int64       | 8 bytes |
| 0x13 | 19   | Decimal128  | 16 bytes |
| 0x7F | 127  | MaxKey      | 0 bytes |
| 0xFF | 255  | MinKey      | 0 bytes |

## JSONB Internal Format

```
[varlena header (4 bytes)]
[container header (4 bytes): flags | element_count]
[JEntry array: N x 4 bytes, each = type_flags | offset_or_length]
[key data (for objects): sorted key strings, no null terminators]
[value data: numeric/string/nested containers]
```

JEntry type flags:
- `0x00000000` — String
- `0x10000000` — Numeric
- `0x20000000` — Boolean (false)
- `0x30000000` — Boolean (true)
- `0x40000000` — Null
- `0x50000000` — Container (nested object/array)

---

## Dependencies

- PostgreSQL 16+ (auto-detected at build time)
- `documentdb_core` extension (for the `bson` type) — only needed for BSON functions
- `libbson` (MongoDB C driver BSON library)

## Building Without Docker

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install build-essential pkg-config postgresql-server-dev-17 libmongoc-dev libbson-dev

# Build and install
cd pg_docinspect
make
sudo make install

# Enable in PostgreSQL
psql -c "CREATE EXTENSION docinspect;"
```

## License

Same as the parent DocumentDB project.
