-- docinspect: Document inspection utilities for BSON and JSONB

-- Create the extension schema
CREATE SCHEMA IF NOT EXISTS docinspect;

-- =============================================================================
-- Helper: Resolve a DocumentDB collection to its underlying table name
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.collection_table(
    p_database_name text,
    p_collection_name text
)
RETURNS regclass
LANGUAGE sql STABLE STRICT
AS $$
    SELECT format('documentdb_data.documents_%s', collection_id)::regclass
    FROM documentdb_api_catalog.collections c
    WHERE c.database_name = p_database_name
      AND c.collection_name = p_collection_name;
$$;

COMMENT ON FUNCTION docinspect.collection_table(text, text)
    IS 'Returns the underlying regclass for a DocumentDB collection (e.g. documentdb_data.documents_3)';

-- =============================================================================
-- Helper: Inspect index entries for a DocumentDB collection document by _id
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.collection_index_entries(
    p_database_name text,
    p_collection_name text,
    p_doc_id documentdb_core.bson
)
RETURNS TABLE (
    index_name   text,
    index_type   text,
    entry_repr   text,
    entry_bytes  bytea
)
LANGUAGE plpgsql STABLE STRICT
AS $$
DECLARE
    tbl regclass;
    target_ctid tid;
BEGIN
    tbl := docinspect.collection_table(p_database_name, p_collection_name);

    -- Find the ctid for the document with the given _id
    EXECUTE format(
        'SELECT ctid FROM %s WHERE object_id = $1 LIMIT 1', tbl
    ) INTO target_ctid USING p_doc_id;

    IF target_ctid IS NULL THEN
        RAISE EXCEPTION 'Document with _id % not found in %.%',
            p_doc_id, p_database_name, p_collection_name;
    END IF;

    RETURN QUERY SELECT * FROM docinspect.index_entries(tbl, target_ctid);
END;
$$;

COMMENT ON FUNCTION docinspect.collection_index_entries(text, text, documentdb_core.bson)
    IS 'Shows all index entries for a document identified by _id in a DocumentDB collection';

-- =============================================================================
-- Helper: Show index terms for a DocumentDB document using bson decoding.
-- For RUM/GIN entries, decodes the raw bytes as BSON for readable output.
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.collection_index_terms(
    p_database_name text,
    p_collection_name text,
    p_doc_id documentdb_core.bson
)
RETURNS TABLE (
    index_name   text,
    index_type   text,
    term_bson    text
)
LANGUAGE plpgsql STABLE STRICT
AS $$
DECLARE
    tbl regclass;
    target_ctid tid;
    idx_rec record;
    decoded text;
BEGIN
    tbl := docinspect.collection_table(p_database_name, p_collection_name);

    -- Find the ctid for the document with the given _id
    EXECUTE format(
        'SELECT ctid FROM %s WHERE object_id = $1 LIMIT 1', tbl
    ) INTO target_ctid USING p_doc_id;

    IF target_ctid IS NULL THEN
        RAISE EXCEPTION 'Document with _id % not found in %.%',
            p_doc_id, p_database_name, p_collection_name;
    END IF;

    -- Get raw index entries and decode them
    FOR idx_rec IN
        SELECT ie.index_name, ie.index_type, ie.entry_repr, ie.entry_bytes
        FROM docinspect.index_entries(tbl, target_ctid) ie
    LOOP
        -- For btree entries, entry_repr is already readable
        IF idx_rec.index_type = 'btree' THEN
            RETURN QUERY SELECT idx_rec.index_name, idx_rec.index_type, idx_rec.entry_repr;
        ELSE
            -- For RUM/GIN, the entry_repr shows the formed datum as hex.
            -- Try to decode it as BSON using the BSONHEX input format.
            IF idx_rec.entry_bytes IS NOT NULL THEN
                BEGIN
                    -- entry_bytes contains the serialized varlena datum.
                    -- For BSON columns, this is [4-byte varlena header][raw BSON bytes].
                    -- Extract just the BSON bytes (skip first 4 bytes = varlena header)
                    -- and use BSONHEX format to parse.
                    decoded := ('BSONHEX' || encode(
                        substring(idx_rec.entry_bytes from 5), 'hex'
                    ))::documentdb_core.bson::text;
                    RETURN QUERY SELECT idx_rec.index_name, idx_rec.index_type, decoded;
                    CONTINUE;
                EXCEPTION WHEN OTHERS THEN
                    NULL; -- fall through
                END;
            END IF;

            -- Fallback: show truncated entry_repr
            RETURN QUERY SELECT
                idx_rec.index_name,
                idx_rec.index_type,
                left(idx_rec.entry_repr, 120) ||
                    CASE WHEN length(idx_rec.entry_repr) > 120 THEN '...' ELSE '' END;
        END IF;
    END LOOP;
END;
$$;

COMMENT ON FUNCTION docinspect.collection_index_terms(text, text, documentdb_core.bson)
    IS 'Shows index terms for a DocumentDB document with human-readable BSON output';

-- =============================================================================
-- BSON Inspection: Shows the binary layout of a BSON document
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.bson_inspect(
    doc documentdb_core.bson
)
RETURNS TABLE (
    depth        int,
    byte_offset  int,
    type_hex     text,
    type_name    text,
    field_name   text,
    byte_length  int,
    value_repr   text
)
LANGUAGE c STRICT
AS 'MODULE_PATHNAME', $function$docinspect_bson_inspect$function$;

COMMENT ON FUNCTION docinspect.bson_inspect(documentdb_core.bson)
    IS 'Inspects the binary layout of a BSON document, showing type/key/value breakdown';

-- =============================================================================
-- BSON Pretty Print
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.bson_inspect_pretty(
    doc documentdb_core.bson
)
RETURNS text
LANGUAGE c STRICT
AS 'MODULE_PATHNAME', $function$docinspect_bson_inspect_pretty$function$;

COMMENT ON FUNCTION docinspect.bson_inspect_pretty(documentdb_core.bson)
    IS 'Returns a pretty-printed text showing the BSON binary layout with indentation';

-- =============================================================================
-- JSONB Inspection
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.jsonb_inspect(
    doc jsonb
)
RETURNS TABLE (
    depth        int,
    byte_offset  int,
    type_name    text,
    key_or_index text,
    byte_length  int,
    value_repr   text
)
LANGUAGE c STRICT
AS 'MODULE_PATHNAME', $function$docinspect_jsonb_inspect$function$;

COMMENT ON FUNCTION docinspect.jsonb_inspect(jsonb)
    IS 'Inspects the binary layout of a JSONB value, showing header/container/entry breakdown';

-- =============================================================================
-- JSONB Pretty Print
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.jsonb_inspect_pretty(
    doc jsonb
)
RETURNS text
LANGUAGE c STRICT
AS 'MODULE_PATHNAME', $function$docinspect_jsonb_inspect_pretty$function$;

COMMENT ON FUNCTION docinspect.jsonb_inspect_pretty(jsonb)
    IS 'Returns a pretty-printed text showing the JSONB binary layout with indentation';

-- =============================================================================
-- Index Entry Inspection (raw): Shows all index entries for a heap tuple
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.index_entries(
    relname regclass,
    ctid tid
)
RETURNS TABLE (
    index_name   text,
    index_type   text,
    entry_repr   text,
    entry_bytes  bytea
)
LANGUAGE c STRICT
AS 'MODULE_PATHNAME', $function$docinspect_index_entries$function$;

COMMENT ON FUNCTION docinspect.index_entries(regclass, tid)
    IS 'Shows all index entries (btree, GIN, RUM) pointing to the given heap tuple';

-- =============================================================================
-- Schema setup
-- =============================================================================

GRANT USAGE ON SCHEMA docinspect TO PUBLIC;

-- =============================================================================
-- Storage Inspection: Shows how a varlena datum is stored (inline/compressed/TOAST)
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.storage_info(
    doc anyelement
)
RETURNS TABLE (
    storage_type      text,
    raw_size          int,
    stored_size       int,
    compression_ratio float8,
    toast_pointer     bool
)
LANGUAGE c STRICT
AS 'MODULE_PATHNAME', $function$docinspect_storage_info$function$;

COMMENT ON FUNCTION docinspect.storage_info(anyelement)
    IS 'Shows storage details: inline/compressed/TOAST, sizes, and compression ratio';

-- =============================================================================
-- TOAST Chunks: Shows TOAST chunk details for a table column value
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.toast_chunks(
    p_relname regclass,
    p_ctid tid,
    p_colname text
)
RETURNS TABLE (
    chunk_id     oid,
    chunk_seq    int,
    chunk_size   int
)
LANGUAGE plpgsql STABLE STRICT
AS $$
DECLARE
    toast_relid oid;
    val_oid oid;
BEGIN
    -- Get the TOAST table for this relation
    SELECT reltoastrelid INTO toast_relid
    FROM pg_class WHERE oid = p_relname;

    IF toast_relid IS NULL OR toast_relid = 0 THEN
        RAISE NOTICE 'Table % has no TOAST table (all values stored inline)', p_relname;
        RETURN;
    END IF;

    -- Get the raw toast pointer value from the tuple
    -- We query the TOAST table directly for all chunks
    RETURN QUERY EXECUTE format(
        'SELECT chunk_id, chunk_seq, octet_length(chunk_data)::int '
        'FROM pg_toast.%I '
        'WHERE chunk_id = (SELECT chunk_id FROM pg_toast.%I LIMIT 1) '
        'ORDER BY chunk_seq',
        'pg_toast_' || p_relname::oid,
        'pg_toast_' || p_relname::oid
    );
END;
$$;

COMMENT ON FUNCTION docinspect.toast_chunks(regclass, tid, text)
    IS 'Shows TOAST chunk details (chunk_id, sequence, size) for a TOASTed value';

-- =============================================================================
-- Storage Summary: Shows storage stats for all rows in a table column
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.storage_summary(
    p_relname regclass,
    p_colname text,
    p_limit int DEFAULT 100
)
RETURNS TABLE (
    total_rows       bigint,
    avg_raw_size     numeric,
    avg_stored_size  numeric,
    min_size         int,
    max_size         int,
    toasted_count    bigint,
    inline_count     bigint,
    avg_compression  numeric
)
LANGUAGE plpgsql STABLE STRICT
AS $$
BEGIN
    RETURN QUERY EXECUTE format(
        'WITH sample AS (
            SELECT %I as val FROM %s LIMIT %s
        ),
        info AS (
            SELECT
                (docinspect.storage_info(val)).raw_size as raw,
                (docinspect.storage_info(val)).stored_size as stored,
                (docinspect.storage_info(val)).toast_pointer as is_toasted
            FROM sample
            WHERE val IS NOT NULL
        )
        SELECT
            count(*)::bigint,
            round(avg(raw)::numeric, 1),
            round(avg(stored)::numeric, 1),
            min(stored),
            max(stored),
            count(*) FILTER (WHERE is_toasted)::bigint,
            count(*) FILTER (WHERE NOT is_toasted)::bigint,
            CASE WHEN avg(raw) > 0
                 THEN round((1.0 - avg(stored)::numeric / avg(raw)::numeric) * 100, 1)
                 ELSE 0 END
        FROM info',
        p_colname, p_relname::text, p_limit
    );
END;
$$;

COMMENT ON FUNCTION docinspect.storage_summary(regclass, text, int)
    IS 'Shows storage statistics for a column: sizes, compression ratio, TOAST counts';

-- =============================================================================
-- Index Stats: Shows index efficiency metrics for a DocumentDB collection
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.index_stats(
    p_database_name text DEFAULT NULL,
    p_collection_name text DEFAULT NULL
)
RETURNS TABLE (
    table_name       text,
    index_name       text,
    index_type       text,
    index_size       text,
    table_size       text,
    index_scans      bigint,
    tuples_read      bigint,
    tuples_fetched   bigint,
    bloat_ratio      numeric
)
LANGUAGE plpgsql STABLE
AS $$
DECLARE
    tbl_pattern text;
BEGIN
    -- Build table name pattern
    IF p_database_name IS NOT NULL AND p_collection_name IS NOT NULL THEN
        tbl_pattern := docinspect.collection_table(p_database_name, p_collection_name)::text;
    ELSE
        tbl_pattern := 'documentdb_data.documents_%';
    END IF;

    RETURN QUERY
    SELECT
        c.relname::text as table_name,
        i.relname::text as index_name,
        am.amname::text as index_type,
        pg_size_pretty(pg_relation_size(i.oid)) as index_size,
        pg_size_pretty(pg_relation_size(c.oid)) as table_size,
        COALESCE(s.idx_scan, 0) as index_scans,
        COALESCE(s.idx_tup_read, 0) as tuples_read,
        COALESCE(s.idx_tup_fetch, 0) as tuples_fetched,
        CASE WHEN pg_relation_size(c.oid) > 0
             THEN round(pg_relation_size(i.oid)::numeric / pg_relation_size(c.oid)::numeric, 2)
             ELSE 0 END as bloat_ratio
    FROM pg_index idx
    JOIN pg_class c ON c.oid = idx.indrelid
    JOIN pg_class i ON i.oid = idx.indexrelid
    JOIN pg_am am ON am.oid = i.relam
    LEFT JOIN pg_stat_user_indexes s ON s.indexrelid = i.oid
    WHERE c.relnamespace = 'documentdb_data'::regnamespace
      AND (p_database_name IS NULL OR c.relname = split_part(tbl_pattern, '.', 2)
           OR tbl_pattern = 'documentdb_data.documents_%')
    ORDER BY pg_relation_size(i.oid) DESC;
END;
$$;

COMMENT ON FUNCTION docinspect.index_stats(text, text)
    IS 'Shows index efficiency metrics: size, scan counts, bloat ratio';

-- =============================================================================
-- Index Size Breakdown: Detailed size info per index for a collection
-- =============================================================================

CREATE OR REPLACE FUNCTION docinspect.index_size_detail(
    p_database_name text,
    p_collection_name text
)
RETURNS TABLE (
    index_name       text,
    index_type       text,
    index_size_bytes bigint,
    index_size_pretty text,
    table_size_bytes bigint,
    ratio_to_table   numeric,
    num_index_tuples bigint
)
LANGUAGE plpgsql STABLE STRICT
AS $$
DECLARE
    tbl regclass;
BEGIN
    tbl := docinspect.collection_table(p_database_name, p_collection_name);

    RETURN QUERY
    SELECT
        i.relname::text,
        am.amname::text,
        pg_relation_size(i.oid),
        pg_size_pretty(pg_relation_size(i.oid)),
        pg_relation_size(tbl),
        CASE WHEN pg_relation_size(tbl) > 0
             THEN round(pg_relation_size(i.oid)::numeric / pg_relation_size(tbl)::numeric * 100, 1)
             ELSE 0 END,
        i.reltuples::bigint
    FROM pg_index idx
    JOIN pg_class i ON i.oid = idx.indexrelid
    JOIN pg_am am ON am.oid = i.relam
    WHERE idx.indrelid = tbl
    ORDER BY pg_relation_size(i.oid) DESC;
END;
$$;

COMMENT ON FUNCTION docinspect.index_size_detail(text, text)
    IS 'Shows detailed size breakdown for all indexes on a DocumentDB collection';
