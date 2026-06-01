-- Test: BSON inspection functions
-- Verifies binary layout output for various BSON document structures

-- Setup
CREATE EXTENSION IF NOT EXISTS documentdb_core;
CREATE EXTENSION IF NOT EXISTS docinspect;

-- =============================================================================
-- Test 1: Simple document with string and int
-- =============================================================================
SELECT * FROM docinspect.bson_inspect('{"name": "Alice", "age": 30}'::documentdb_core.bson);

-- =============================================================================
-- Test 2: Pretty print simple document
-- =============================================================================
SELECT docinspect.bson_inspect_pretty('{"a": 1, "b": "hello"}'::documentdb_core.bson);

-- =============================================================================
-- Test 3: Nested document
-- =============================================================================
SELECT depth, type_hex, type_name, field_name, byte_length
FROM docinspect.bson_inspect('{"outer": {"inner": 42}}'::documentdb_core.bson);

-- =============================================================================
-- Test 4: Array
-- =============================================================================
SELECT depth, type_hex, type_name, field_name, value_repr
FROM docinspect.bson_inspect('{"arr": [1, 2, 3]}'::documentdb_core.bson);

-- =============================================================================
-- Test 5: Multiple types
-- =============================================================================
SELECT type_name, field_name, value_repr
FROM docinspect.bson_inspect(
  '{"s": "text", "i": 42, "f": 3.14, "b": true, "n": null}'::documentdb_core.bson
)
WHERE type_name NOT IN ('DocHeader', 'EOD');

-- =============================================================================
-- Test 6: Empty document
-- =============================================================================
SELECT * FROM docinspect.bson_inspect('{}'::documentdb_core.bson);

-- =============================================================================
-- Test 7: Deeply nested
-- =============================================================================
SELECT depth, type_name, field_name
FROM docinspect.bson_inspect('{"a": {"b": {"c": {"d": 1}}}}'::documentdb_core.bson)
WHERE type_name NOT IN ('EOD');

-- =============================================================================
-- Test 8: Pretty print with array
-- =============================================================================
SELECT docinspect.bson_inspect_pretty('{"tags": ["x", "y"], "count": 2}'::documentdb_core.bson);
