/*-------------------------------------------------------------------------
 *
 * bson_inspect.c
 *
 * Functions to inspect the binary layout of BSON documents.
 * Shows type-length-key-value breakdown with depth for nested structures.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <utils/builtins.h>
#include <lib/stringinfo.h>

#include <bson.h>

#include "io/pgbson.h"

PG_FUNCTION_INFO_V1(docinspect_bson_inspect);
PG_FUNCTION_INFO_V1(docinspect_bson_inspect_pretty);


/*
 * BSON type code to human-readable name mapping.
 */
static const char *
bson_type_name(bson_type_t type)
{
	switch (type)
	{
		case BSON_TYPE_EOD:        return "EOD";
		case BSON_TYPE_DOUBLE:     return "Double";
		case BSON_TYPE_UTF8:       return "String";
		case BSON_TYPE_DOCUMENT:   return "Document";
		case BSON_TYPE_ARRAY:      return "Array";
		case BSON_TYPE_BINARY:     return "Binary";
		case BSON_TYPE_UNDEFINED:  return "Undefined";
		case BSON_TYPE_OID:        return "ObjectId";
		case BSON_TYPE_BOOL:       return "Boolean";
		case BSON_TYPE_DATE_TIME:  return "DateTime";
		case BSON_TYPE_NULL:       return "Null";
		case BSON_TYPE_REGEX:      return "Regex";
		case BSON_TYPE_DBPOINTER: return "DBPointer";
		case BSON_TYPE_CODE:       return "Code";
		case BSON_TYPE_SYMBOL:     return "Symbol";
		case BSON_TYPE_CODEWSCOPE: return "CodeWScope";
		case BSON_TYPE_INT32:      return "Int32";
		case BSON_TYPE_TIMESTAMP:  return "Timestamp";
		case BSON_TYPE_INT64:      return "Int64";
		case BSON_TYPE_DECIMAL128: return "Decimal128";
		case BSON_TYPE_MAXKEY:     return "MaxKey";
		case BSON_TYPE_MINKEY:     return "MinKey";
		default:                   return "Unknown";
	}
}


/*
 * Get the byte size of a BSON value (the value portion only, not type+key).
 */
static int
bson_value_byte_size(const bson_iter_t *iter)
{
	bson_type_t type = bson_iter_type(iter);

	switch (type)
	{
		case BSON_TYPE_DOUBLE:     return 8;
		case BSON_TYPE_BOOL:       return 1;
		case BSON_TYPE_INT32:      return 4;
		case BSON_TYPE_INT64:      return 8;
		case BSON_TYPE_TIMESTAMP:  return 8;
		case BSON_TYPE_DATE_TIME:  return 8;
		case BSON_TYPE_OID:        return 12;
		case BSON_TYPE_NULL:       return 0;
		case BSON_TYPE_UNDEFINED:  return 0;
		case BSON_TYPE_MINKEY:     return 0;
		case BSON_TYPE_MAXKEY:     return 0;
		case BSON_TYPE_DECIMAL128: return 16;

		case BSON_TYPE_UTF8:
			/* 4 bytes length prefix + string + null terminator */
			return 4 + bson_iter_utf8_len_unsafe(iter) + 1;

		case BSON_TYPE_BINARY:
		{
			uint32_t len;
			bson_subtype_t subtype;
			const uint8_t *data;
			bson_iter_binary(iter, &subtype, &len, &data);
			/* 4 bytes length + 1 byte subtype + data */
			return 4 + 1 + len;
		}

		case BSON_TYPE_DOCUMENT:
		case BSON_TYPE_ARRAY:
		{
			const uint8_t *buf;
			uint32_t len;
			if (type == BSON_TYPE_DOCUMENT)
				bson_iter_document(iter, &len, &buf);
			else
				bson_iter_array(iter, &len, &buf);
			return (int) len;
		}

		case BSON_TYPE_REGEX:
		{
			const char *regex = bson_iter_regex(iter, NULL);
			const char *opts;
			bson_iter_regex(iter, &opts);
			/* regex string + null + options string + null */
			return (int)(strlen(regex) + 1 + strlen(opts) + 1);
		}

		case BSON_TYPE_CODE:
		{
			uint32_t len;
			bson_iter_code(iter, &len);
			return 4 + len + 1;
		}

		case BSON_TYPE_CODEWSCOPE:
		{
			uint32_t code_len;
			uint32_t scope_len;
			const uint8_t *scope;
			bson_iter_codewscope(iter, &code_len, &scope_len, &scope);
			/* 4 total_len + 4 code_len + code + null + scope_doc */
			return 4 + 4 + code_len + 1 + scope_len;
		}

		default:
			return -1;  /* unknown */
	}
}


/*
 * Format a BSON value as a human-readable string.
 */
static char *
bson_value_repr(const bson_iter_t *iter)
{
	bson_type_t type = bson_iter_type(iter);
	StringInfoData buf;
	initStringInfo(&buf);

	switch (type)
	{
		case BSON_TYPE_DOUBLE:
			appendStringInfo(&buf, "%g", bson_iter_double(iter));
			break;

		case BSON_TYPE_UTF8:
		{
			uint32_t len;
			const char *str = bson_iter_utf8(iter, &len);
			if (len > 64)
				appendStringInfo(&buf, "\"%.*s\"... (%u bytes)", 64, str, len);
			else
				appendStringInfo(&buf, "\"%s\"", str);
			break;
		}

		case BSON_TYPE_DOCUMENT:
		{
			uint32_t len;
			const uint8_t *data;
			bson_iter_document(iter, &len, &data);
			appendStringInfo(&buf, "{...} (%u bytes)", len);
			break;
		}

		case BSON_TYPE_ARRAY:
		{
			uint32_t len;
			const uint8_t *data;
			bson_iter_array(iter, &len, &data);
			appendStringInfo(&buf, "[...] (%u bytes)", len);
			break;
		}

		case BSON_TYPE_BINARY:
		{
			uint32_t len;
			bson_subtype_t subtype;
			const uint8_t *data;
			bson_iter_binary(iter, &subtype, &len, &data);
			appendStringInfo(&buf, "Binary(subtype=0x%02x, %u bytes)", subtype, len);
			break;
		}

		case BSON_TYPE_OID:
		{
			char oid_str[25];
			bson_oid_to_string(bson_iter_oid(iter), oid_str);
			appendStringInfo(&buf, "ObjectId(\"%s\")", oid_str);
			break;
		}

		case BSON_TYPE_BOOL:
			appendStringInfo(&buf, "%s", bson_iter_bool(iter) ? "true" : "false");
			break;

		case BSON_TYPE_DATE_TIME:
			appendStringInfo(&buf, "DateTime(%lld)", (long long) bson_iter_date_time(iter));
			break;

		case BSON_TYPE_NULL:
			appendStringInfoString(&buf, "null");
			break;

		case BSON_TYPE_REGEX:
		{
			const char *opts;
			const char *regex = bson_iter_regex(iter, &opts);
			appendStringInfo(&buf, "/%s/%s", regex, opts);
			break;
		}

		case BSON_TYPE_INT32:
			appendStringInfo(&buf, "%d", bson_iter_int32(iter));
			break;

		case BSON_TYPE_INT64:
			appendStringInfo(&buf, "%lld", (long long) bson_iter_int64(iter));
			break;

		case BSON_TYPE_TIMESTAMP:
		{
			uint32_t t, i;
			bson_iter_timestamp(iter, &t, &i);
			appendStringInfo(&buf, "Timestamp(%u, %u)", t, i);
			break;
		}

		case BSON_TYPE_DECIMAL128:
		{
			bson_decimal128_t dec;
			char dec_str[BSON_DECIMAL128_STRING];
			bson_iter_decimal128(iter, &dec);
			bson_decimal128_to_string(&dec, dec_str);
			appendStringInfo(&buf, "Decimal128(\"%s\")", dec_str);
			break;
		}

		case BSON_TYPE_UNDEFINED:
			appendStringInfoString(&buf, "undefined");
			break;

		case BSON_TYPE_MINKEY:
			appendStringInfoString(&buf, "MinKey");
			break;

		case BSON_TYPE_MAXKEY:
			appendStringInfoString(&buf, "MaxKey");
			break;

		case BSON_TYPE_CODE:
		{
			uint32_t len;
			const char *code = bson_iter_code(iter, &len);
			if (len > 64)
				appendStringInfo(&buf, "Code(\"%.*s\"...)", 64, code);
			else
				appendStringInfo(&buf, "Code(\"%s\")", code);
			break;
		}

		default:
			appendStringInfo(&buf, "<type 0x%02x>", type);
			break;
	}

	return buf.data;
}


/*
 * Recursive state for bson_inspect SRF.
 */
typedef struct BsonInspectEntry
{
	int depth;
	int byte_offset;
	uint8_t type_code;
	const char *type_name;
	char *field_name;
	int byte_length;
	char *value_repr;
} BsonInspectEntry;

typedef struct BsonInspectState
{
	BsonInspectEntry *entries;
	int num_entries;
	int max_entries;
	int current_entry;
} BsonInspectState;


static void
ensure_entry_capacity(BsonInspectState *state)
{
	if (state->num_entries >= state->max_entries)
	{
		state->max_entries *= 2;
		state->entries = repalloc(state->entries,
								 sizeof(BsonInspectEntry) * state->max_entries);
	}
}


/*
 * Recursively walk a BSON document and collect entries.
 * base_offset is the byte offset of this document within the top-level document.
 */
static void
collect_bson_entries(const uint8_t *data, uint32_t data_len,
					int depth, int base_offset,
					BsonInspectState *state)
{
	bson_t bson;
	bson_iter_t iter;

	if (!bson_init_static(&bson, data, data_len))
		return;

	/* Add document header entry (4-byte length prefix) */
	ensure_entry_capacity(state);
	BsonInspectEntry *hdr = &state->entries[state->num_entries++];
	hdr->depth = depth;
	hdr->byte_offset = base_offset;
	hdr->type_code = 0xFF;  /* synthetic: document header */
	hdr->type_name = "DocHeader";
	hdr->field_name = psprintf("size=%u", data_len);
	hdr->byte_length = 4;
	hdr->value_repr = psprintf("%u bytes total (includes 4-byte length + 1-byte terminator)",
							   data_len);

	if (!bson_iter_init(&iter, &bson))
		return;

	while (bson_iter_next(&iter))
	{
		bson_type_t type = bson_iter_type(&iter);
		const char *key = bson_iter_key(&iter);

		/*
		 * Calculate the offset of this element within the raw data.
		 * In BSON, each element is: 1 byte type + key (null-terminated) + value
		 * The iter offset gives us the position of the type byte.
		 */
		int elem_offset = base_offset + (int)(bson_iter_offset(&iter));
		int key_len = (int) strlen(key) + 1;  /* includes null terminator */
		int val_size = bson_value_byte_size(&iter);
		int total_elem_size = 1 + key_len + (val_size >= 0 ? val_size : 0);

		ensure_entry_capacity(state);
		BsonInspectEntry *entry = &state->entries[state->num_entries++];
		entry->depth = depth;
		entry->byte_offset = elem_offset;
		entry->type_code = (uint8_t) type;
		entry->type_name = bson_type_name(type);
		entry->field_name = pstrdup(key);
		entry->byte_length = total_elem_size;
		entry->value_repr = bson_value_repr(&iter);

		/* Recurse into sub-documents and arrays */
		if (type == BSON_TYPE_DOCUMENT || type == BSON_TYPE_ARRAY)
		{
			const uint8_t *sub_data;
			uint32_t sub_len;
			if (type == BSON_TYPE_DOCUMENT)
				bson_iter_document(&iter, &sub_len, &sub_data);
			else
				bson_iter_array(&iter, &sub_len, &sub_data);

			/* Value starts after type byte + key + null */
			int value_offset = elem_offset + 1 + key_len;
			collect_bson_entries(sub_data, sub_len, depth + 1, value_offset, state);
		}
	}

	/* Add document terminator entry */
	ensure_entry_capacity(state);
	BsonInspectEntry *term = &state->entries[state->num_entries++];
	term->depth = depth;
	term->byte_offset = base_offset + (int) data_len - 1;
	term->type_code = 0x00;
	term->type_name = "EOD";
	term->field_name = pstrdup("");
	term->byte_length = 1;
	term->value_repr = pstrdup("0x00 terminator");
}


/*
 * docinspect_bson_inspect - SRF returning the binary layout of a BSON document.
 */
Datum
docinspect_bson_inspect(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	BsonInspectState *state;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Build tuple descriptor: depth, byte_offset, type_hex, type_name, field_name, byte_length, value_repr */
		tupdesc = CreateTemplateTupleDesc(7);
		TupleDescInitEntry(tupdesc, 1, "depth", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "byte_offset", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "type_hex", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 4, "type_name", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 5, "field_name", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 6, "byte_length", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 7, "value_repr", TEXTOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/* Parse the BSON document and collect all entries */
		pgbson *doc = PG_GETARG_PGBSON(0);
		const uint8_t *data = (const uint8_t *) VARDATA_ANY(doc);
		uint32_t data_len = VARSIZE_ANY_EXHDR(doc);

		state = palloc0(sizeof(BsonInspectState));
		state->max_entries = 64;
		state->entries = palloc(sizeof(BsonInspectEntry) * state->max_entries);
		state->num_entries = 0;
		state->current_entry = 0;

		collect_bson_entries(data, data_len, 0, 0, state);

		funcctx->user_fctx = state;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (BsonInspectState *) funcctx->user_fctx;

	if (state->current_entry < state->num_entries)
	{
		BsonInspectEntry *entry = &state->entries[state->current_entry++];
		Datum values[7];
		bool nulls[7] = {false};
		HeapTuple tuple;
		char hex_buf[8];

		snprintf(hex_buf, sizeof(hex_buf), "0x%02x", entry->type_code);

		values[0] = Int32GetDatum(entry->depth);
		values[1] = Int32GetDatum(entry->byte_offset);
		values[2] = CStringGetTextDatum(hex_buf);
		values[3] = CStringGetTextDatum(entry->type_name);
		values[4] = CStringGetTextDatum(entry->field_name);
		values[5] = Int32GetDatum(entry->byte_length);
		values[6] = CStringGetTextDatum(entry->value_repr);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}


/*
 * docinspect_bson_inspect_pretty - Returns a formatted text representation.
 */
Datum
docinspect_bson_inspect_pretty(PG_FUNCTION_ARGS)
{
	pgbson *doc = PG_GETARG_PGBSON(0);
	const uint8_t *data = (const uint8_t *) VARDATA_ANY(doc);
	uint32_t data_len = VARSIZE_ANY_EXHDR(doc);

	BsonInspectState state;
	state.max_entries = 64;
	state.entries = palloc(sizeof(BsonInspectEntry) * state.max_entries);
	state.num_entries = 0;
	state.current_entry = 0;

	collect_bson_entries(data, data_len, 0, 0, &state);

	StringInfoData result;
	initStringInfo(&result);

	appendStringInfo(&result, "BSON Document (%u bytes)\n", data_len);
	appendStringInfoString(&result, "========================================\n");

	for (int i = 0; i < state.num_entries; i++)
	{
		BsonInspectEntry *e = &state.entries[i];

		/* Indentation based on depth */
		for (int d = 0; d < e->depth; d++)
			appendStringInfoString(&result, "  ");

		appendStringInfo(&result, "[%04d] 0x%02x %-12s",
						 e->byte_offset, e->type_code, e->type_name);

		if (e->field_name[0] != '\0')
			appendStringInfo(&result, " \"%s\"", e->field_name);

		appendStringInfo(&result, "  (%d bytes)", e->byte_length);

		if (e->value_repr[0] != '\0' &&
			e->type_code != 0xFF &&  /* skip doc header value in compact view */
			e->type_code != 0x03 &&  /* skip document value (shown via recursion) */
			e->type_code != 0x04)    /* skip array value (shown via recursion) */
		{
			appendStringInfo(&result, "  = %s", e->value_repr);
		}

		appendStringInfoChar(&result, '\n');
	}

	PG_RETURN_TEXT_P(cstring_to_text(result.data));
}
