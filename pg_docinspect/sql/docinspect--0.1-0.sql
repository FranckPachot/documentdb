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
