/*-------------------------------------------------------------------------
 *
 * docinspect.c
 *
 * Entry point for the docinspect extension.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <fmgr.h>
#include <miscadmin.h>

PG_MODULE_MAGIC;

void _PG_init(void);

void
_PG_init(void)
{
	/* Nothing to initialize for now */
}
