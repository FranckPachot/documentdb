/*-------------------------------------------------------------------------
 *
 * toast_inspect.c
 *
 * Functions to inspect TOAST storage details for a varlena datum:
 * - Whether it's stored inline, compressed, or externally (TOASTed)
 * - Compression ratio
 * - Raw vs stored size
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <utils/builtins.h>
#include <lib/stringinfo.h>

#if PG_VERSION_NUM >= 160000
#include <varatt.h>
#endif

PG_FUNCTION_INFO_V1(docinspect_storage_info);


/*
 * docinspect_storage_info - Shows storage details for a varlena datum.
 *
 * Takes any varlena type (text, bytea, jsonb, bson, etc.) and reports
 * how it's physically stored: inline, compressed, or in the TOAST table.
 *
 * Returns a composite: (storage_type, raw_size, stored_size, compression_ratio, toast_pointer)
 */
Datum
docinspect_storage_info(PG_FUNCTION_ARGS)
{
	TupleDesc tupdesc;
	Datum values[5];
	bool nulls[5] = {false, false, false, false, false};
	HeapTuple tuple;

	/* Build result tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(5);
	TupleDescInitEntry(tupdesc, 1, "storage_type", TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, 2, "raw_size", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, 3, "stored_size", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, 4, "compression_ratio", FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, 5, "toast_pointer", BOOLOID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	/*
	 * Get the raw varlena pointer WITHOUT detoasting.
	 * PG_GETARG_RAW_VARLENA_P gives us the on-disk representation.
	 */
	struct varlena *raw = PG_GETARG_RAW_VARLENA_P(0);
	const char *storage_type;
	int raw_size;
	int stored_size;
	bool is_toast_pointer = false;

	/*
	 * Determine storage type from the varlena header.
	 */
	if (VARATT_IS_EXTERNAL(raw))
	{
		storage_type = "external (TOAST table)";
		is_toast_pointer = true;
		stored_size = VARSIZE_EXTERNAL(raw);
		/* Detoast to get the real uncompressed size */
		struct varlena *detoasted = (struct varlena *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
		raw_size = (int) VARSIZE_ANY_EXHDR(detoasted);
		if (detoasted != raw)
			pfree(detoasted);
	}
	else if (VARATT_IS_COMPRESSED(raw))
	{
		storage_type = "inline (compressed)";
		stored_size = (int) VARSIZE_ANY(raw);
		struct varlena *detoasted = (struct varlena *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
		raw_size = (int) VARSIZE_ANY_EXHDR(detoasted);
		if (detoasted != raw)
			pfree(detoasted);
	}
	else if (VARATT_IS_SHORT(raw))
	{
		storage_type = "inline (short, 1-byte header)";
		raw_size = (int) VARSIZE_SHORT(raw) - VARHDRSZ_SHORT;
		stored_size = (int) VARSIZE_SHORT(raw);
	}
	else
	{
		storage_type = "inline (4-byte header)";
		raw_size = (int) VARSIZE_ANY_EXHDR(raw);
		stored_size = (int) VARSIZE_ANY(raw);
	}

	double ratio = (raw_size > 0) ? (double) stored_size / (double) raw_size : 1.0;

	values[0] = CStringGetTextDatum(storage_type);
	values[1] = Int32GetDatum(raw_size);
	values[2] = Int32GetDatum(stored_size);
	values[3] = Float8GetDatum(ratio);
	values[4] = BoolGetDatum(is_toast_pointer);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
