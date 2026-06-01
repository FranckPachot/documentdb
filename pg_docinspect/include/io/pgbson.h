/*-------------------------------------------------------------------------
 *
 * pgbson.h (standalone shim for pg_docinspect)
 *
 * Minimal definition of the pgbson type so we can detoast and access
 * the raw BSON bytes without depending on the full documentdb_core headers.
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_DOCINSPECT_PGBSON_H
#define PG_DOCINSPECT_PGBSON_H

#include <postgres.h>

#if PG_VERSION_NUM >= 160000
#include <varatt.h>
#endif

/*
 * pgbson is a varlena: 4-byte length header followed by raw BSON bytes.
 * This matches the definition in documentdb_core.
 */
typedef struct
{
	int32 vl_len_;
	char vl_dat[FLEXIBLE_ARRAY_MEMBER];
} pgbson;

#define DatumGetPgBson(n) ((pgbson *) PG_DETOAST_DATUM(n))
#define PG_GETARG_PGBSON(n) (DatumGetPgBson(PG_GETARG_DATUM(n)))

#endif /* PG_DOCINSPECT_PGBSON_H */
