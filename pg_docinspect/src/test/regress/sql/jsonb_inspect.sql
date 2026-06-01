-- Test: JSONB inspection functions
-- Verifies internal structure output for various JSONB values

-- Setup
CREATE EXTENSION IF NOT EXISTS docinspect;

-- =============================================================================
-- Test 1: Simple object
-- =============================================================================
SELECT * FROM docinspect.jsonb_inspect('{"name": "Alice", "age": 30}'::jsonb);

-- =============================================================================
-- Test 2: Pretty print simple object
-- =============================================================================
SELECT docinspect.jsonb_inspect_pretty('{"a": 1, "b": "hello"}'::jsonb);

-- =============================================================================
-- Test 3: Key ordering (JSONB sorts keys alphabetically)
-- =============================================================================
SELECT type_name, key_or_index, value_repr
FROM docinspect.jsonb_inspect('{"z": 1, "a": 2, "m": 3}'::jsonb)
WHERE type_name = 'Key';

-- =============================================================================
-- Test 4: Array
-- =============================================================================
SELECT type_name, key_or_index, value_repr
FROM docinspect.jsonb_inspect('[1, "two", true, null]'::jsonb)
WHERE type_name NOT IN ('JSONB Header', 'Array', 'EndArray');

-- =============================================================================
-- Test 5: Nested object
-- =============================================================================
SELECT depth, type_name, key_or_index, value_repr
FROM docinspect.jsonb_inspect('{"outer": {"inner": 42}}'::jsonb);

-- =============================================================================
-- Test 6: Mixed types
-- =============================================================================
SELECT type_name, key_or_index, value_repr
FROM docinspect.jsonb_inspect('{"s": "text", "n": 42, "b": true, "x": null}'::jsonb)
WHERE type_name NOT IN ('JSONB Header', 'Object', 'EndObject', 'Key');

-- =============================================================================
-- Test 7: Empty object and array
-- =============================================================================
SELECT docinspect.jsonb_inspect_pretty('{}'::jsonb);
SELECT docinspect.jsonb_inspect_pretty('[]'::jsonb);

-- =============================================================================
-- Test 8: Pretty print nested
-- =============================================================================
SELECT docinspect.jsonb_inspect_pretty('{"users": [{"name": "Bob"}, {"name": "Eve"}]}'::jsonb);
