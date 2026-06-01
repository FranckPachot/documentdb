-- Test: Storage inspection functions
-- Verifies TOAST detection, compression info, and storage summary

-- Setup
CREATE EXTENSION IF NOT EXISTS docinspect;

-- =============================================================================
-- Test 1: Small inline value (no TOAST)
-- =============================================================================
SELECT (docinspect.storage_info('{"a": 1}'::jsonb)).storage_type;
SELECT (docinspect.storage_info('{"a": 1}'::jsonb)).toast_pointer;

-- =============================================================================
-- Test 2: Storage info for a small text value
-- =============================================================================
SELECT (docinspect.storage_info('hello world'::text)).storage_type;
SELECT (docinspect.storage_info('hello world'::text)).raw_size;

-- =============================================================================
-- Test 3: Storage summary on a test table
-- =============================================================================
CREATE TABLE test_storage (
    id serial PRIMARY KEY,
    data jsonb
);

INSERT INTO test_storage (data)
SELECT jsonb_build_object(
    'key', i,
    'value', repeat('x', 50)
)
FROM generate_series(1, 10) i;

SELECT * FROM docinspect.storage_summary('test_storage'::regclass, 'data', 10);

-- =============================================================================
-- Test 4: Large values that get TOASTed
-- =============================================================================
INSERT INTO test_storage (data)
SELECT jsonb_build_object(
    'key', i,
    'big_value', repeat('abcdefghij', 1000)
)
FROM generate_series(11, 15) i;

-- Check that large values are detected
SELECT
    (docinspect.storage_info(data)).storage_type,
    (docinspect.storage_info(data)).raw_size,
    (docinspect.storage_info(data)).stored_size,
    (docinspect.storage_info(data)).toast_pointer
FROM test_storage
WHERE id = 11;

-- =============================================================================
-- Test 5: Storage summary with mixed sizes
-- =============================================================================
SELECT * FROM docinspect.storage_summary('test_storage'::regclass, 'data', 15);

-- =============================================================================
-- Test 6: TOAST chunks for the table
-- =============================================================================
SELECT * FROM docinspect.toast_chunks('test_storage'::regclass, '(0,11)'::tid, 'data');

-- Cleanup
DROP TABLE test_storage;
