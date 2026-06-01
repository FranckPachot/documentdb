/*-------------------------------------------------------------------------
 *
 * jsonb_inspect.c
 *
 * Functions to inspect the binary layout of PostgreSQL JSONB values.
 * Uses the JsonbIterator API to reliably walk the internal structure
 * and report containers, keys, and values with byte offsets.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <utils/builtins.h>
#include <utils/jsonb.h>
#include <utils/numeric.h>
#include <lib/stringinfo.h>

#if PG_VERSION_NUM >= 160000
#include <varatt.h>
#endif

PG_FUNCTION_INFO_V1(docinspect_jsonb_inspect);
PG_FUNCTION_INFO_V1(docinspect_jsonb_inspect_pretty);


typedef struct JsonbInspectEntry
{
	int depth;
	int byte_offset;
	char *type_name;
	char *key_or_index;
	int byte_length;
	char *value_repr;
} JsonbInspectEntry;

typedef struct JsonbInspectState
{
	JsonbInspectEntry *entries;
	int num_entries;
	int max_entries;
	int current_entry;
} JsonbInspectState;


static void
ensure_jsonb_capacity(JsonbInspectState *state)
{
	if (state->num_entries >= state->max_entries)
	{
		state->max_entries *= 2;
		state->entries = (JsonbInspectEntry *) repalloc(state->entries,
								 sizeof(JsonbInspectEntry) * state->max_entries);
	}
}


/*
 * Format a scalar JsonbValue as a human-readable string.
 */
static char *
format_jsonb_scalar(JsonbValue *val)
{
	switch (val->type)
	{
		case jbvNull:
			return pstrdup("null");

		case jbvBool:
			return pstrdup(val->val.boolean ? "true" : "false");

		case jbvNumeric:
		{
			Datum d = NumericGetDatum(val->val.numeric);
			char *numstr = DatumGetCString(DirectFunctionCall1(numeric_out, d));
			return numstr;
		}

		case jbvString:
		{
			int len = val->val.string.len;
			if (len > 64)
				return psprintf("\"%.*s\"... (%d bytes)", 64, val->val.string.val, len);
			else
				return psprintf("\"%.*s\"", len, val->val.string.val);
		}

		default:
			return psprintf("<type %d>", val->type);
	}
}


/*
 * Get the approximate byte size of a scalar JsonbValue's data portion.
 */
static int
jsonb_scalar_size(JsonbValue *val)
{
	switch (val->type)
	{
		case jbvNull:
			return 0;  /* encoded in JEntry flags only */
		case jbvBool:
			return 0;  /* encoded in JEntry flags only */
		case jbvNumeric:
			return (int) VARSIZE_ANY(val->val.numeric);
		case jbvString:
			return val->val.string.len;
		default:
			return 0;
	}
}


/*
 * Walk the JSONB using JsonbIterator and collect entries.
 * This is safe — we only access val fields appropriate for each token type.
 */
static void
collect_jsonb_entries(Jsonb *jb, JsonbInspectState *state)
{
	JsonbIterator *it;
	JsonbValue val;
	JsonbIteratorToken tok;
	int depth = 0;
	int offset = 0;
	char *pending_key = NULL;
	int array_index[32];
	bool in_array[32];

	memset(array_index, 0, sizeof(array_index));
	memset(in_array, 0, sizeof(in_array));

	/* Total size after varlena header */
	int total_size = (int) VARSIZE(jb) - VARHDRSZ;

	/* Add overall header entry */
	ensure_jsonb_capacity(state);
	JsonbInspectEntry *hdr = &state->entries[state->num_entries++];
	hdr->depth = 0;
	hdr->byte_offset = 0;
	hdr->type_name = pstrdup("JSONB Header");
	hdr->key_or_index = pstrdup("");
	hdr->byte_length = total_size;
	hdr->value_repr = psprintf("%d bytes total (after varlena header)", total_size);

	it = JsonbIteratorInit(&jb->root);

	while ((tok = JsonbIteratorNext(&it, &val, false)) != WJB_DONE)
	{
		switch (tok)
		{
			case WJB_BEGIN_OBJECT:
			{
				ensure_jsonb_capacity(state);
				JsonbInspectEntry *e = &state->entries[state->num_entries++];
				e->depth = depth;
				e->byte_offset = offset;
				e->type_name = pstrdup("Object");

				if (pending_key)
				{
					e->key_or_index = pending_key;
					pending_key = NULL;
				}
				else if (depth > 0 && in_array[depth - 1])
				{
					e->key_or_index = psprintf("[%d]", array_index[depth - 1]++);
				}
				else
				{
					e->key_or_index = pstrdup("");
				}

				/*
				 * For WJB_BEGIN_OBJECT, val.type is jbvObject but we cannot
				 * safely access val.val.binary. Just note the container header.
				 */
				e->byte_length = 4;  /* container header is 4 bytes */
				e->value_repr = psprintf("container header (4 bytes)");

				/* Advance offset past the header; JEntry sizes are unknown here */
				offset += 4;

				if (depth < 32)
				{
					in_array[depth] = false;
					array_index[depth] = 0;
				}
				depth++;
				break;
			}

			case WJB_BEGIN_ARRAY:
			{
				ensure_jsonb_capacity(state);
				JsonbInspectEntry *e = &state->entries[state->num_entries++];
				e->depth = depth;
				e->byte_offset = offset;
				e->type_name = pstrdup("Array");

				if (pending_key)
				{
					e->key_or_index = pending_key;
					pending_key = NULL;
				}
				else if (depth > 0 && in_array[depth - 1])
				{
					e->key_or_index = psprintf("[%d]", array_index[depth - 1]++);
				}
				else
				{
					e->key_or_index = pstrdup("");
				}

				e->byte_length = 4;
				e->value_repr = psprintf("container header (4 bytes)");

				offset += 4;

				if (depth < 32)
				{
					in_array[depth] = true;
					array_index[depth] = 0;
				}
				depth++;
				break;
			}

			case WJB_END_OBJECT:
			case WJB_END_ARRAY:
			{
				depth--;
				ensure_jsonb_capacity(state);
				JsonbInspectEntry *e = &state->entries[state->num_entries++];
				e->depth = depth;
				e->byte_offset = offset;
				e->type_name = pstrdup(tok == WJB_END_OBJECT ? "EndObject" : "EndArray");
				e->key_or_index = pstrdup("");
				e->byte_length = 0;
				e->value_repr = pstrdup("");
				break;
			}

			case WJB_KEY:
			{
				/* Key: val.type == jbvString, safe to access val.val.string */
				int key_len = val.val.string.len;

				ensure_jsonb_capacity(state);
				JsonbInspectEntry *e = &state->entries[state->num_entries++];
				e->depth = depth;
				e->byte_offset = offset;
				e->type_name = pstrdup("Key");
				e->key_or_index = pstrdup("");
				e->byte_length = key_len;
				e->value_repr = psprintf("\"%.*s\"", key_len, val.val.string.val);

				pending_key = pnstrdup(val.val.string.val, key_len);
				offset += key_len;
				break;
			}

			case WJB_VALUE:
			case WJB_ELEM:
			{
				int val_size = jsonb_scalar_size(&val);

				ensure_jsonb_capacity(state);
				JsonbInspectEntry *e = &state->entries[state->num_entries++];
				e->depth = depth;
				e->byte_offset = offset;
				e->byte_length = val_size;
				e->value_repr = format_jsonb_scalar(&val);

				/* Determine type name */
				switch (val.type)
				{
					case jbvNull:
						e->type_name = pstrdup("Null");
						break;
					case jbvBool:
						e->type_name = pstrdup("Boolean");
						break;
					case jbvNumeric:
						e->type_name = pstrdup("Numeric");
						break;
					case jbvString:
						e->type_name = pstrdup("String");
						break;
					case jbvBinary:
						/* Nested container as a value — will be iterated into */
						e->type_name = pstrdup("Container");
						val_size = 0; /* offset handled by WJB_BEGIN_* */
						e->byte_length = 0;
						e->value_repr = pstrdup("{...} or [...]");
						break;
					default:
						e->type_name = psprintf("Type(%d)", val.type);
						break;
				}

				/* Key or index context */
				if (tok == WJB_VALUE && pending_key)
				{
					e->key_or_index = pending_key;
					pending_key = NULL;
				}
				else if (tok == WJB_ELEM && depth > 0 && depth <= 32 && in_array[depth - 1])
				{
					e->key_or_index = psprintf("[%d]", array_index[depth - 1]++);
				}
				else
				{
					e->key_or_index = pstrdup("");
				}

				offset += val_size;
				break;
			}

			default:
				break;
		}
	}
}


/*
 * docinspect_jsonb_inspect - SRF returning the binary layout of a JSONB value.
 */
Datum
docinspect_jsonb_inspect(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	JsonbInspectState *state;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* depth, byte_offset, type_name, key_or_index, byte_length, value_repr */
		tupdesc = CreateTemplateTupleDesc(6);
		TupleDescInitEntry(tupdesc, 1, "depth", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "byte_offset", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "type_name", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 4, "key_or_index", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 5, "byte_length", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 6, "value_repr", TEXTOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		Jsonb *jb = PG_GETARG_JSONB_P(0);

		state = (JsonbInspectState *) palloc0(sizeof(JsonbInspectState));
		state->max_entries = 64;
		state->entries = (JsonbInspectEntry *) palloc(sizeof(JsonbInspectEntry) * state->max_entries);
		state->num_entries = 0;
		state->current_entry = 0;

		collect_jsonb_entries(jb, state);

		funcctx->user_fctx = state;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (JsonbInspectState *) funcctx->user_fctx;

	if (state->current_entry < state->num_entries)
	{
		JsonbInspectEntry *entry = &state->entries[state->current_entry++];
		Datum values[6];
		bool nulls[6] = {false, false, false, false, false, false};
		HeapTuple tuple;

		values[0] = Int32GetDatum(entry->depth);
		values[1] = Int32GetDatum(entry->byte_offset);
		values[2] = CStringGetTextDatum(entry->type_name);
		values[3] = CStringGetTextDatum(entry->key_or_index);
		values[4] = Int32GetDatum(entry->byte_length);
		values[5] = CStringGetTextDatum(entry->value_repr);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}


/*
 * docinspect_jsonb_inspect_pretty - Returns formatted text of JSONB layout.
 */
Datum
docinspect_jsonb_inspect_pretty(PG_FUNCTION_ARGS)
{
	Jsonb *jb = PG_GETARG_JSONB_P(0);

	JsonbInspectState state;
	state.max_entries = 64;
	state.entries = (JsonbInspectEntry *) palloc(sizeof(JsonbInspectEntry) * state.max_entries);
	state.num_entries = 0;
	state.current_entry = 0;

	collect_jsonb_entries(jb, &state);

	StringInfoData result;
	initStringInfo(&result);

	int total_size = (int) VARSIZE(jb) - VARHDRSZ;
	appendStringInfo(&result, "JSONB Value (%d bytes after varlena header)\n", total_size);
	appendStringInfoString(&result, "========================================\n");

	for (int i = 0; i < state.num_entries; i++)
	{
		JsonbInspectEntry *e = &state.entries[i];

		for (int d = 0; d < e->depth; d++)
			appendStringInfoString(&result, "  ");

		appendStringInfo(&result, "[%04d] %-12s", e->byte_offset, e->type_name);

		if (e->key_or_index[0] != '\0')
			appendStringInfo(&result, " %s", e->key_or_index);

		if (e->byte_length > 0)
			appendStringInfo(&result, "  (%d bytes)", e->byte_length);

		if (e->value_repr[0] != '\0')
			appendStringInfo(&result, "  = %s", e->value_repr);

		appendStringInfoChar(&result, '\n');
	}

	PG_RETURN_TEXT_P(cstring_to_text(result.data));
}
