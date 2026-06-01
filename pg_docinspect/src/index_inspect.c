/*-------------------------------------------------------------------------
 *
 * index_inspect.c
 *
 * Functions to inspect index entries (btree, GIN, RUM) pointing to a
 * given heap tuple. Similar in spirit to pageinspect but focused on
 * showing which index entries reference a specific document.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <access/genam.h>
#include <access/heapam.h>
#include <access/htup_details.h>
#include <access/nbtree.h>
#include <access/relation.h>
#include <access/table.h>
#include <access/tableam.h>
#include <catalog/index.h>
#include <catalog/indexing.h>
#include <catalog/namespace.h>
#include <catalog/pg_am.h>
#include <catalog/pg_index.h>
#include <executor/executor.h>
#include <nodes/execnodes.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/fmgroids.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/snapmgr.h>
#include <utils/syscache.h>
#include <lib/stringinfo.h>

#if PG_VERSION_NUM >= 160000
#include <varatt.h>
#endif

PG_FUNCTION_INFO_V1(docinspect_index_entries);


typedef struct IndexEntryResult
{
	char *index_name;
	char *index_type;
	char *entry_repr;
	bytea *entry_bytes;
} IndexEntryResult;

typedef struct IndexEntriesState
{
	IndexEntryResult *results;
	int num_results;
	int max_results;
	int current_result;
} IndexEntriesState;


static void
ensure_result_capacity(IndexEntriesState *state)
{
	if (state->num_results >= state->max_results)
	{
		state->max_results *= 2;
		state->results = repalloc(state->results,
								  sizeof(IndexEntryResult) * state->max_results);
	}
}


/*
 * Get the access method name for an index relation.
 * Uses the pg_am catalog via syscache (works on all PG versions).
 */
static char *
get_index_am_name(Relation indexRel)
{
	HeapTuple amTuple;
	Form_pg_am amForm;
	char *am_name;

	amTuple = SearchSysCache1(AMOID, ObjectIdGetDatum(indexRel->rd_rel->relam));
	if (!HeapTupleIsValid(amTuple))
		return pstrdup("unknown");

	amForm = (Form_pg_am) GETSTRUCT(amTuple);
	am_name = pstrdup(NameStr(amForm->amname));
	ReleaseSysCache(amTuple);

	return am_name;
}


/*
 * Format an index tuple's key values as a human-readable string.
 */
static char *
format_index_entry(Relation indexRel, Datum *values, bool *isnull, int natts)
{
	StringInfoData buf;
	initStringInfo(&buf);

	appendStringInfoChar(&buf, '(');
	for (int i = 0; i < natts; i++)
	{
		if (i > 0)
			appendStringInfoString(&buf, ", ");

		if (isnull[i])
		{
			appendStringInfoString(&buf, "NULL");
		}
		else
		{
			Oid typoid = TupleDescAttr(RelationGetDescr(indexRel), i)->atttypid;
			Oid outfunc;
			bool typIsVarlena;
			getTypeOutputInfo(typoid, &outfunc, &typIsVarlena);

			char *val_str = OidOutputFunctionCall(outfunc, values[i]);
			/* Truncate long values for readability */
			if (strlen(val_str) > 100)
			{
				val_str[97] = '.';
				val_str[98] = '.';
				val_str[99] = '.';
				val_str[100] = '\0';
			}
			appendStringInfoString(&buf, val_str);
			pfree(val_str);
		}
	}
	appendStringInfoChar(&buf, ')');

	return buf.data;
}


/*
 * Serialize index entry values to bytea for raw inspection.
 */
static bytea *
serialize_index_entry(Relation indexRel, Datum *values, bool *isnull, int natts)
{
	StringInfoData buf;
	initStringInfo(&buf);

	for (int i = 0; i < natts; i++)
	{
		if (!isnull[i])
		{
			Oid typoid = TupleDescAttr(RelationGetDescr(indexRel), i)->atttypid;
			int16 typlen;
			bool typbyval;
			char typalign;

			get_typlenbyvalalign(typoid, &typlen, &typbyval, &typalign);

			if (typbyval)
			{
				size_t len = (typlen > 0) ? (size_t) typlen : sizeof(Datum);
				appendBinaryStringInfo(&buf, (char *) &values[i], (int) len);
			}
			else if (typlen == -1)
			{
				/* varlena */
				void *detoasted = (void *) PG_DETOAST_DATUM(values[i]);
				appendBinaryStringInfo(&buf, (char *) detoasted, VARSIZE(detoasted));
			}
			else if (typlen == -2)
			{
				/* cstring */
				char *s = DatumGetCString(values[i]);
				appendBinaryStringInfo(&buf, s, (int) strlen(s) + 1);
			}
			else
			{
				appendBinaryStringInfo(&buf, DatumGetPointer(values[i]), typlen);
			}
		}
	}

	bytea *result = (bytea *) palloc(VARHDRSZ + buf.len);
	SET_VARSIZE(result, VARHDRSZ + buf.len);
	memcpy(VARDATA(result), buf.data, buf.len);
	pfree(buf.data);

	return result;
}


/*
 * Collect index entries for a given heap tuple from a specific index.
 * Uses the index's extractvalue support function for GIN/RUM indexes,
 * or shows the formed index datum for btree indexes.
 */
static void
collect_entries_from_index(Relation heapRel, Relation indexRel,
						   HeapTuple heapTuple, IndexEntriesState *state)
{
	TupleDesc heapDesc = RelationGetDescr(heapRel);
	TupleDesc indexDesc = RelationGetDescr(indexRel);
	int natts = indexDesc->natts;
	Datum *values = (Datum *) palloc(sizeof(Datum) * natts);
	bool *isnull = (bool *) palloc(sizeof(bool) * natts);

	/* Get the AM name via syscache */
	char *am_name = get_index_am_name(indexRel);
	char *idx_name = pstrdup(RelationGetRelationName(indexRel));

	/*
	 * Use FormIndexDatum to extract the index key values from the heap tuple.
	 * This gives us what would be stored in the index for this tuple.
	 */
	EState *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);

	/* Set up the slot for the heap tuple */
	TupleTableSlot *slot = MakeSingleTupleTableSlot(heapDesc, &TTSOpsHeapTuple);
	ExecStoreHeapTuple(heapTuple, slot, false);
	econtext->ecxt_scantuple = slot;

	FormIndexDatum(BuildIndexInfo(indexRel), slot, estate, values, isnull);

	/* For btree-like indexes, we get a single entry per tuple */
	if (strcmp(am_name, "btree") == 0)
	{
		ensure_result_capacity(state);
		IndexEntryResult *r = &state->results[state->num_results++];
		r->index_name = idx_name;
		r->index_type = pstrdup("btree");
		r->entry_repr = format_index_entry(indexRel, values, isnull, natts);
		r->entry_bytes = serialize_index_entry(indexRel, values, isnull, natts);
	}
	else if (strcmp(am_name, "gin") == 0 || strcmp(am_name, "rum") == 0)
	{
		/*
		 * For GIN/RUM indexes, the extractvalue function returns multiple
		 * index entries per document. We call it to get the actual terms.
		 *
		 * The GIN extractvalue support function (support number 2) signature:
		 *   Datum *extractValue(Datum itemValue, int32 *nkeys, bool **nullFlags)
		 */
		for (int col = 0; col < natts; col++)
		{
			if (isnull[col])
				continue;

			/* Look up the extractvalue support function for this column */
			Oid opfamily = indexRel->rd_opfamily[col];
			Oid opcintype = indexRel->rd_opcintype[col];
			Oid extractProc = get_opfamily_proc(opfamily, opcintype, opcintype, 2);

			if (!OidIsValid(extractProc))
			{
				/* No extractvalue function; treat as single entry */
				ensure_result_capacity(state);
				IndexEntryResult *r = &state->results[state->num_results++];
				r->index_name = idx_name;
				r->index_type = pstrdup(am_name);
				r->entry_repr = format_index_entry(indexRel, values, isnull, natts);
				r->entry_bytes = serialize_index_entry(indexRel, values, isnull, natts);
				continue;
			}

			/* Call extractvalue */
			int32 nkeys = 0;
			bool *nullFlags = NULL;

			Datum *entries = (Datum *)
				DatumGetPointer(OidFunctionCall3(extractProc,
												values[col],
												PointerGetDatum(&nkeys),
												PointerGetDatum(&nullFlags)));

			for (int32 k = 0; k < nkeys; k++)
			{
				ensure_result_capacity(state);
				IndexEntryResult *r = &state->results[state->num_results++];
				r->index_name = idx_name;
				r->index_type = pstrdup(am_name);

				if (nullFlags && nullFlags[k])
				{
					r->entry_repr = psprintf("col%d: NULL (term %d/%d)", col, k + 1, nkeys);
					r->entry_bytes = NULL;
				}
				else
				{
					/* Format the individual term */
					Oid termtype = TupleDescAttr(indexDesc, col)->atttypid;
					Oid outfunc;
					bool typIsVarlena;
					getTypeOutputInfo(termtype, &outfunc, &typIsVarlena);

					char *term_str = OidOutputFunctionCall(outfunc, entries[k]);
					if (strlen(term_str) > 200)
					{
						term_str[197] = '.';
						term_str[198] = '.';
						term_str[199] = '.';
						term_str[200] = '\0';
					}
					r->entry_repr = psprintf("col%d term[%d/%d]: %s",
											 col, k + 1, nkeys, term_str);

					/* Serialize the term */
					int16 typlen;
					bool typbyval;
					char typalign;
					get_typlenbyvalalign(termtype, &typlen, &typbyval, &typalign);

					if (typbyval)
					{
						r->entry_bytes = (bytea *) palloc(VARHDRSZ + sizeof(Datum));
						SET_VARSIZE(r->entry_bytes, VARHDRSZ + sizeof(Datum));
						memcpy(VARDATA(r->entry_bytes), &entries[k], sizeof(Datum));
					}
					else if (typlen == -1)
					{
						void *detoasted = (void *) PG_DETOAST_DATUM(entries[k]);
						int sz = VARSIZE(detoasted);
						r->entry_bytes = (bytea *) palloc(VARHDRSZ + sz);
						SET_VARSIZE(r->entry_bytes, VARHDRSZ + sz);
						memcpy(VARDATA(r->entry_bytes), detoasted, sz);
					}
					else
					{
						r->entry_bytes = NULL;
					}

					pfree(term_str);
				}
			}
		}
	}
	else
	{
		/* Other index types: just show the formed datum */
		ensure_result_capacity(state);
		IndexEntryResult *r = &state->results[state->num_results++];
		r->index_name = idx_name;
		r->index_type = pstrdup(am_name);
		r->entry_repr = format_index_entry(indexRel, values, isnull, natts);
		r->entry_bytes = serialize_index_entry(indexRel, values, isnull, natts);
	}

	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	pfree(values);
	pfree(isnull);
	pfree(am_name);
}


/*
 * docinspect_index_entries - SRF showing all index entries for a heap tuple.
 */
Datum
docinspect_index_entries(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	IndexEntriesState *state;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Build tuple descriptor */
		tupdesc = CreateTemplateTupleDesc(4);
		TupleDescInitEntry(tupdesc, 1, "index_name", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "index_type", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "entry_repr", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, 4, "entry_bytes", BYTEAOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/* Get arguments */
		Oid relid = PG_GETARG_OID(0);
		ItemPointer tid = PG_GETARG_ITEMPOINTER(1);

		state = (IndexEntriesState *) palloc0(sizeof(IndexEntriesState));
		state->max_results = 32;
		state->results = (IndexEntryResult *) palloc(sizeof(IndexEntryResult) * state->max_results);
		state->num_results = 0;
		state->current_result = 0;

		/* Open the heap relation and fetch the tuple */
		Relation heapRel = table_open(relid, AccessShareLock);
		Snapshot snapshot = GetActiveSnapshot();
		HeapTuple heapTuple = NULL;

		/* Fetch the heap tuple by TID using table AM */
		{
			TupleTableSlot *slot = table_slot_create(heapRel, NULL);
			if (table_tuple_fetch_row_version(heapRel, tid, snapshot, slot))
			{
				bool shouldFree;
				heapTuple = ExecFetchSlotHeapTuple(slot, false, &shouldFree);
				heapTuple = heap_copytuple(heapTuple);
			}
			ExecDropSingleTupleTableSlot(slot);
		}

		if (heapTuple == NULL)
		{
			table_close(heapRel, AccessShareLock);
			ereport(ERROR,
					(errcode(ERRCODE_NO_DATA_FOUND),
					 errmsg("tuple not found at tid (%u,%u)",
							ItemPointerGetBlockNumber(tid),
							ItemPointerGetOffsetNumber(tid))));
		}

		/* Find all indexes on this relation */
		List *indexOids = RelationGetIndexList(heapRel);
		ListCell *lc;

		foreach(lc, indexOids)
		{
			Oid indexOid = lfirst_oid(lc);
			Relation indexRel = index_open(indexOid, AccessShareLock);

			/* Only process valid, ready indexes */
			if (indexRel->rd_index->indisvalid && indexRel->rd_index->indisready)
			{
				collect_entries_from_index(heapRel, indexRel, heapTuple, state);
			}

			index_close(indexRel, AccessShareLock);
		}

		heap_freetuple(heapTuple);
		list_free(indexOids);
		table_close(heapRel, AccessShareLock);

		funcctx->user_fctx = state;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (IndexEntriesState *) funcctx->user_fctx;

	if (state->current_result < state->num_results)
	{
		IndexEntryResult *r = &state->results[state->current_result++];
		Datum values[4];
		bool nulls[4] = {false, false, false, false};
		HeapTuple tuple;

		values[0] = CStringGetTextDatum(r->index_name);
		values[1] = CStringGetTextDatum(r->index_type);
		values[2] = CStringGetTextDatum(r->entry_repr);

		if (r->entry_bytes)
			values[3] = PointerGetDatum(r->entry_bytes);
		else
			nulls[3] = true;

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}
