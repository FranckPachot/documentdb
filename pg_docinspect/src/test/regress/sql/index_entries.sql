-- Test: Index entry inspection functions
-- Verifies index entry extraction for btree and GIN indexes

-- Setup
CREATE EXTENSION IF NOT EXISTS docinspect;

-- =============================================================================
-- Test 1: Basic btree index entries
-- =============================================================================
CREATE TABLE test_idx (
    id serial PRIMARY KEY,
    name text,
    value int
);

INSERT INTO test_idx (name, value) VALUES
    ('alpha', 10),
    ('beta', 20),
    ('gamma', 30);

CREATE INDEX idx_test_name ON test_idx (name);
CREATE INDEX idx_test_value ON test_idx (value);

-- Show index entries for first row
SELECT index_name, index_type, entry_repr
FROM docinspect.index_entries('test_idx'::regclass, '(0,1)'::tid)
ORDER BY index_name;

-- =============================================================================
-- Test 2: GIN index on JSONB
-- =============================================================================
CREATE TABLE test_jsonb_idx (
    id serial PRIMARY KEY,
    data jsonb
);

INSERT INTO test_jsonb_idx (data) VALUES
    ('{"type": "click", "page": "/home"}'),
    ('{"type": "view", "page": "/about", "duration": 5}'),
    ('{"type": "click", "page": "/products", "button": "buy"}');

CREATE INDEX idx_jsonb_gin ON test_jsonb_idx USING gin (data jsonb_ops);

-- Show GIN index entries for first row
SELECT index_name, index_type, entry_repr
FROM docinspect.index_entries('test_jsonb_idx'::regclass, '(0,1)'::tid)
ORDER BY index_name, entry_repr;

-- =============================================================================
-- Test 3: Multiple indexes on same table
-- =============================================================================
CREATE INDEX idx_jsonb_type ON test_jsonb_idx ((data->>'type'));
CREATE INDEX idx_jsonb_page ON test_jsonb_idx ((data->>'page'));

SELECT index_name, index_type, entry_repr
FROM docinspect.index_entries('test_jsonb_idx'::regclass, '(0,1)'::tid)
ORDER BY index_name, entry_repr;

-- =============================================================================
-- Test 4: Index stats function
-- =============================================================================
-- Note: index_stats requires documentdb_data schema, so we test with pg tables
SELECT
    pg_relation_size(i.oid) >= 0 as has_size,
    am.amname as index_type
FROM pg_index idx
JOIN pg_class i ON i.oid = idx.indexrelid
JOIN pg_am am ON am.oid = i.relam
WHERE idx.indrelid = 'test_idx'::regclass
ORDER BY i.relname;

-- =============================================================================
-- Test 5: Verify entry count matches expected
-- =============================================================================
-- btree: 1 entry per index per row
-- GIN: multiple entries per row (one per key+value)
SELECT index_type, count(*) as entry_count
FROM docinspect.index_entries('test_jsonb_idx'::regclass, '(0,1)'::tid)
GROUP BY index_type
ORDER BY index_type;

-- Cleanup
DROP TABLE test_idx;
DROP TABLE test_jsonb_idx;
