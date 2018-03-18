/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This file contains the sqlite3_get_table() and sqlite3_free_table()
 * interface routines.  These are just wrappers around the main
 * interface routine of sqlite3_exec().
 *
 * These routines are in a separate files so that they will not be linked
 * if they are not used.
 */
#include "sqliteInt.h"
#include <stdlib.h>
#include <string.h>

#ifndef SQLITE_OMIT_GET_TABLE

/*
 * This structure is used to pass data from sqlite3_get_table() through
 * to the callback function is uses to build the result.
 */
typedef struct TabResult {
	char **azResult;	/* Accumulated output */
	char *zErrMsg;		/* Error message text, if an error occurs */
	u32 nAlloc;		/* Slots allocated for azResult[] */
	u32 nRow;		/* Number of rows in the result */
	u32 nColumn;		/* Number of columns in the result */
	u32 nData;		/* Slots used in azResult[].  (nRow+1)*nColumn */
	int rc;			/* Return code from sqlite3_exec() */
} TabResult;


/*
 * Query the database.  But instead of invoking a callback for each row,
 * malloc() for space to hold the result and return the entire results
 * at the conclusion of the call.
 *
 * The result that is written to ***pazResult is held in memory obtained
 * from malloc().  But the caller cannot free this memory directly.
 * Instead, the entire table should be passed to sqlite3_free_table() when
 * the calling procedure is finished using it.
 */

/*
 * This routine frees the space the sqlite3_get_table() malloced.
 */

#endif				/* SQLITE_OMIT_GET_TABLE */
