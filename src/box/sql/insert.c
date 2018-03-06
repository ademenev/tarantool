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
 * This file contains C code routines that are called by the parser
 * to handle INSERT statements in SQLite.
 */
#include "sqliteInt.h"
#include "tarantoolInt.h"
#include "box/session.h"
#include "box/schema.h"

/*
 * Generate code that will open pTab as cursor iCur.
 */
void
sqlite3OpenTable(Parse * pParse,	/* Generate code into this VDBE */
		 int iCur,	/* The cursor number of the table */
		 Table * pTab,	/* The table to be opened */
		 int opcode)	/* OP_OpenRead or OP_OpenWrite */
{
	Vdbe *v;
	v = sqlite3GetVdbe(pParse);
	assert(opcode == OP_OpenWrite || opcode == OP_OpenRead);
	Index *pPk = sqlite3PrimaryKeyIndex(pTab);
	assert(pPk != 0);
	assert(pPk->tnum == pTab->tnum);
	struct space *space = space_by_id(SQLITE_PAGENO_TO_SPACEID(pPk->tnum));
	assert(space != NULL);
	int space_ptr_reg = ++pParse->nMem;
	sqlite3VdbeAddOp4Int64(v, OP_Int64, 0, space_ptr_reg, 0,
			       ((int64_t) space));
	sqlite3VdbeAddOp3(v, opcode, iCur, pPk->tnum, space_ptr_reg);
	sqlite3VdbeSetP4KeyInfo(pParse, pPk);
	VdbeComment((v, "%s", pTab->zName));
}

/*
 * Return a pointer to the column affinity string associated with index
 * pIdx. A column affinity string has one character for each column in
 * the table, according to the affinity of the column:
 *
 *  Character      Column affinity
 *  ------------------------------
 *  'A'            BLOB
 *  'B'            TEXT
 *  'C'            NUMERIC
 *  'D'            INTEGER
 *  'F'            REAL
 *
 * Memory for the buffer containing the column index affinity string
 * is managed along with the rest of the Index structure. It will be
 * released when sqlite3DeleteIndex() is called.
 */
const char *
sqlite3IndexAffinityStr(sqlite3 * db, Index * pIdx)
{
	if (!pIdx->zColAff) {
		/* The first time a column affinity string for a particular index is
		 * required, it is allocated and populated here. It is then stored as
		 * a member of the Index structure for subsequent use.
		 *
		 * The column affinity string will eventually be deleted by
		 * sqliteDeleteIndex() when the Index structure itself is cleaned
		 * up.
		 */
		int n;
		Table *pTab = pIdx->pTable;
		pIdx->zColAff =
		    (char *)sqlite3DbMallocRaw(0, pIdx->nColumn + 1);
		if (!pIdx->zColAff) {
			sqlite3OomFault(db);
			return 0;
		}
		for (n = 0; n < pIdx->nColumn; n++) {
			i16 x = pIdx->aiColumn[n];
			if (x >= 0) {
				pIdx->zColAff[n] = pTab->aCol[x].affinity;
			} else {
				char aff;
				assert(x == XN_EXPR);
				assert(pIdx->aColExpr != 0);
				aff =
				    sqlite3ExprAffinity(pIdx->aColExpr->a[n].
							pExpr);
				if (aff == 0)
					aff = SQLITE_AFF_BLOB;
				pIdx->zColAff[n] = aff;
			}
		}
		pIdx->zColAff[n] = 0;
	}

	return pIdx->zColAff;
}

/*
 * Compute the affinity string for table pTab, if it has not already been
 * computed.  As an optimization, omit trailing SQLITE_AFF_BLOB affinities.
 *
 * If the affinity exists (if it is no entirely SQLITE_AFF_BLOB values) and
 * if iReg>0 then code an OP_Affinity opcode that will set the affinities
 * for register iReg and following.  Or if affinities exists and iReg==0,
 * then just set the P4 operand of the previous opcode (which should  be
 * an OP_MakeRecord) to the affinity string.
 *
 * A column affinity string has one character per column:
 *
 *  Character      Column affinity
 *  ------------------------------
 *  'A'            BLOB
 *  'B'            TEXT
 *  'C'            NUMERIC
 *  'D'            INTEGER
 *  'E'            REAL
 */
void
sqlite3TableAffinity(Vdbe * v, Table * pTab, int iReg)
{
	int i;
	char *zColAff = pTab->zColAff;
	if (zColAff == 0) {
		sqlite3 *db = sqlite3VdbeDb(v);
		zColAff = (char *)sqlite3DbMallocRaw(0, pTab->nCol + 1);
		if (!zColAff) {
			sqlite3OomFault(db);
			return;
		}

		for (i = 0; i < pTab->nCol; i++) {
			zColAff[i] = pTab->aCol[i].affinity;
		}
		do {
			zColAff[i--] = 0;
		} while (i >= 0 && zColAff[i] == SQLITE_AFF_BLOB);
		pTab->zColAff = zColAff;
	}
	i = sqlite3Strlen30(zColAff);
	if (i) {
		if (iReg) {
			sqlite3VdbeAddOp4(v, OP_Affinity, iReg, i, 0, zColAff,
					  i);
		} else {
			sqlite3VdbeChangeP4(v, -1, zColAff, i);
		}
	}
}

/*
 * Return non-zero if the table pTab in database or any of its indices
 * have been opened at any point in the VDBE program. This is used to see if
 * a statement of the form  "INSERT INTO <pTab> SELECT ..." can
 * run for the results of the SELECT.
 */
static int
readsTable(Parse * p, Table * pTab)
{
	Vdbe *v = sqlite3GetVdbe(p);
	int i;
	int iEnd = sqlite3VdbeCurrentAddr(v);

	for (i = 1; i < iEnd; i++) {
		VdbeOp *pOp = sqlite3VdbeGetOp(v, i);
		assert(pOp != 0);
		if (pOp->opcode == OP_OpenRead) {
			Index *pIndex;
			int tnum = pOp->p2;
			if (tnum == pTab->tnum) {
				return 1;
			}
			for (pIndex = pTab->pIndex; pIndex;
			     pIndex = pIndex->pNext) {
				if (tnum == pIndex->tnum) {
					return 1;
				}
			}
		}
	}
	return 0;
}


/* Forward declaration */
static int
xferOptimization(Parse * pParse,	/* Parser context */
		 Table * pDest,	/* The table we are inserting into */
		 Select * pSelect,	/* A SELECT statement to use as the data source */
		 int onError);	/* How to handle constraint errors */

/*
 * This routine is called to handle SQL of the following forms:
 *
 *    insert into TABLE (IDLIST) values(EXPRLIST),(EXPRLIST),...
 *    insert into TABLE (IDLIST) select
 *    insert into TABLE (IDLIST) default values
 *
 * The IDLIST following the table name is always optional.  If omitted,
 * then a list of all columns for the table is substituted.
 * The IDLIST appears in the pColumn parameter.  pColumn is NULL if IDLIST
 * is omitted.
 *
 * For the pSelect parameter holds the values to be inserted for the
 * first two forms shown above.  A VALUES clause is really just short-hand
 * for a SELECT statement that omits the FROM clause and everything else
 * that follows.  If the pSelect parameter is NULL, that means that the
 * DEFAULT VALUES form of the INSERT statement is intended.
 *
 * The code generated follows one of four templates.  For a simple
 * insert with data coming from a single-row VALUES clause, the code executes
 * once straight down through.  Pseudo-code follows (we call this
 * the "1st template"):
 *
 *         open write cursor to <table> and its indices
 *         put VALUES clause expressions into registers
 *         write the resulting record into <table>
 *         cleanup
 *
 * The three remaining templates assume the statement is of the form
 *
 *   INSERT INTO <table> SELECT ...
 *
 * If the SELECT clause is of the restricted form "SELECT * FROM <table2>" -
 * in other words if the SELECT pulls all columns from a single table
 * and there is no WHERE or LIMIT or GROUP BY or ORDER BY clauses, and
 * if <table2> and <table1> are distinct tables but have identical
 * schemas, including all the same indices, then a special optimization
 * is invoked that copies raw records from <table2> over to <table1>.
 * See the xferOptimization() function for the implementation of this
 * template.  This is the 2nd template.
 *
 *         open a write cursor to <table>
 *         open read cursor on <table2>
 *         transfer all records in <table2> over to <table>
 *         close cursors
 *         foreach index on <table>
 *           open a write cursor on the <table> index
 *           open a read cursor on the corresponding <table2> index
 *           transfer all records from the read to the write cursors
 *           close cursors
 *         end foreach
 *
 * The 3rd template is for when the second template does not apply
 * and the SELECT clause does not read from <table> at any time.
 * The generated code follows this template:
 *
 *         X <- A
 *         goto B
 *      A: setup for the SELECT
 *         loop over the rows in the SELECT
 *           load values into registers R..R+n
 *           yield X
 *         end loop
 *         cleanup after the SELECT
 *         end-coroutine X
 *      B: open write cursor to <table> and its indices
 *      C: yield X, at EOF goto D
 *         insert the select result into <table> from R..R+n
 *         goto C
 *      D: cleanup
 *
 * The 4th template is used if the insert statement takes its
 * values from a SELECT but the data is being inserted into a table
 * that is also read as part of the SELECT.  In the third form,
 * we have to use an intermediate table to store the results of
 * the select.  The template is like this:
 *
 *         X <- A
 *         goto B
 *      A: setup for the SELECT
 *         loop over the tables in the SELECT
 *           load value into register R..R+n
 *           yield X
 *         end loop
 *         cleanup after the SELECT
 *         end co-routine R
 *      B: open temp table
 *      L: yield X, at EOF goto M
 *         insert row from R..R+n into temp table
 *         goto L
 *      M: open write cursor to <table> and its indices
 *         rewind temp table
 *      C: loop over rows of intermediate table
 *           transfer values form intermediate table into <table>
 *         end loop
 *      D: cleanup
 */
void
sqlite3Insert(Parse * pParse,	/* Parser context */
	      SrcList * pTabList,	/* Name of table into which we are inserting */
	      Select * pSelect,	/* A SELECT statement to use as the data source */
	      IdList * pColumn,	/* Column names corresponding to IDLIST. */
	      int onError)	/* How to handle constraint errors */
{
	sqlite3 *db;		/* The main database structure */
	Table *pTab;		/* The table to insert into.  aka TABLE */
	char *zTab;		/* Name of the table into which we are inserting */
	int i, j;		/* Loop counters */
	Vdbe *v;		/* Generate code into this virtual machine */
	Index *pIdx;		/* For looping over indices of the table */
	int nColumn;		/* Number of columns in the data */
	int iDataCur = 0;	/* VDBE cursor that is the main data repository */
	int iIdxCur = 0;	/* First index cursor */
	int ipkColumn = -1;	/* Column that is the INTEGER PRIMARY KEY */
	int endOfLoop;		/* Label for the end of the insertion loop */
	int srcTab = 0;		/* Data comes from this temporary cursor if >=0 */
	int addrInsTop = 0;	/* Jump to label "D" */
	int addrCont = 0;	/* Top of insert loop. Label "C" in templates 3 and 4 */
	SelectDest dest;	/* Destination for SELECT on rhs of INSERT */
	u8 useTempTable = 0;	/* Store SELECT results in intermediate table */
	u8 bIdListInOrder;	/* True if IDLIST is in table order */
	ExprList *pList = 0;	/* List of VALUES() to be inserted  */
	struct session *user_session = current_session();

	/* Register allocations */
	int regFromSelect = 0;	/* Base register for data coming from SELECT */
	int regRowCount = 0;	/* Memory cell used for the row counter */
	int regIns;		/* Block of regs holding data being inserted */
	int regTupleid;		/* registers holding insert tupleid */
	int regData;		/* register holding first column to insert */
	int *aRegIdx = 0;	/* One register allocated to each index */

#ifndef SQLITE_OMIT_TRIGGER
	int isView;		/* True if attempting to insert into a view */
	Trigger *pTrigger;	/* List of triggers on pTab, if required */
	int tmask;		/* Mask of trigger times */
#endif

	db = pParse->db;
	memset(&dest, 0, sizeof(dest));
	if (pParse->nErr || db->mallocFailed) {
		goto insert_cleanup;
	}

	/* If the Select object is really just a simple VALUES() list with a
	 * single row (the common case) then keep that one row of values
	 * and discard the other (unused) parts of the pSelect object
	 */
	if (pSelect && (pSelect->selFlags & SF_Values) != 0
	    && pSelect->pPrior == 0) {
		pList = pSelect->pEList;
		pSelect->pEList = 0;
		sqlite3SelectDelete(db, pSelect);
		pSelect = 0;
	}

	/* Locate the table into which we will be inserting new information.
	 */
	assert(pTabList->nSrc == 1);
	zTab = pTabList->a[0].zName;
	if (NEVER(zTab == 0))
		goto insert_cleanup;
	pTab = sqlite3SrcListLookup(pParse, pTabList);
	if (pTab == 0) {
		goto insert_cleanup;
	}

	/* Figure out if we have any triggers and if the table being
	 * inserted into is a view
	 */
#ifndef SQLITE_OMIT_TRIGGER
	pTrigger = sqlite3TriggersExist(pTab, TK_INSERT, 0, &tmask);
	isView = pTab->pSelect != 0;
#else
#define pTrigger 0
#define tmask 0
#define isView 0
#endif
#ifdef SQLITE_OMIT_VIEW
#undef isView
#define isView 0
#endif
	assert((pTrigger && tmask) || (pTrigger == 0 && tmask == 0));

	/* If pTab is really a view, make sure it has been initialized.
	 * ViewGetColumnNames() is a no-op if pTab is not a view.
	 */
	if (sqlite3ViewGetColumnNames(pParse, pTab)) {
		goto insert_cleanup;
	}

	/* Cannot insert into a read-only table.
	 */
	if (sqlite3IsReadOnly(pParse, pTab, tmask)) {
		goto insert_cleanup;
	}

	/* Allocate a VDBE
	 */
	v = sqlite3GetVdbe(pParse);
	if (v == 0)
		goto insert_cleanup;
	if (pParse->nested == 0)
		sqlite3VdbeCountChanges(v);
	sqlite3BeginWriteOperation(pParse, pSelect || pTrigger);

#ifndef SQLITE_OMIT_XFER_OPT
	/* If the statement is of the form
	 *
	 *       INSERT INTO <table1> SELECT * FROM <table2>;
	 *
	 * Then special optimizations can be applied that make the transfer
	 * very fast and which reduce fragmentation of indices.
	 *
	 * This is the 2nd template.
	 */
	if (pColumn == 0 && xferOptimization(pParse, pTab, pSelect, onError)) {
		assert(!pTrigger);
		assert(pList == 0);
		goto insert_end;
	}
#endif				/* SQLITE_OMIT_XFER_OPT */

	/* Allocate registers for holding the tupleid of the new row,
	 * the content of the new row, and the assembled row record.
	 */
	regTupleid = regIns = pParse->nMem + 1;
	pParse->nMem += pTab->nCol + 1;
	regData = regTupleid + 1;

	/* If the INSERT statement included an IDLIST term, then make sure
	 * all elements of the IDLIST really are columns of the table and
	 * remember the column indices.
	 *
	 * If the table has an INTEGER PRIMARY KEY column and that column
	 * is named in the IDLIST, then record in the ipkColumn variable
	 * the index into IDLIST of the primary key column.  ipkColumn is
	 * the index of the primary key as it appears in IDLIST, not as
	 * is appears in the original table.  (The index of the INTEGER
	 * PRIMARY KEY in the original table is pTab->iPKey.)
	 */
	bIdListInOrder = 1;
	if (pColumn) {
		for (i = 0; i < pColumn->nId; i++) {
			pColumn->a[i].idx = -1;
		}
		for (i = 0; i < pColumn->nId; i++) {
			for (j = 0; j < pTab->nCol; j++) {
				if (strcmp
				    (pColumn->a[i].zName,
				     pTab->aCol[j].zName) == 0) {
					pColumn->a[i].idx = j;
					if (i != j)
						bIdListInOrder = 0;
					if (j == pTab->iPKey) {
						ipkColumn = i;
						assert(isView);
					}
					break;
				}
			}
			if (j >= pTab->nCol) {
				sqlite3ErrorMsg(pParse,
						"table %S has no column named %s",
						pTabList, 0, pColumn->a[i].zName);
				pParse->checkSchema = 1;
				goto insert_cleanup;
			}
		}
	}

	/* Figure out how many columns of data are supplied.  If the data
	 * is coming from a SELECT statement, then generate a co-routine that
	 * produces a single row of the SELECT on each invocation.  The
	 * co-routine is the common header to the 3rd and 4th templates.
	 */
	if (pSelect) {
		/* Data is coming from a SELECT or from a multi-row VALUES clause.
		 * Generate a co-routine to run the SELECT.
		 */
		int regYield;	/* Register holding co-routine entry-point */
		int addrTop;	/* Top of the co-routine */
		int rc;		/* Result code */

		regYield = ++pParse->nMem;
		addrTop = sqlite3VdbeCurrentAddr(v) + 1;
		sqlite3VdbeAddOp3(v, OP_InitCoroutine, regYield, 0, addrTop);
		sqlite3SelectDestInit(&dest, SRT_Coroutine, regYield);
		dest.iSdst = bIdListInOrder ? regData : 0;
		dest.nSdst = pTab->nCol;
		rc = sqlite3Select(pParse, pSelect, &dest);
		regFromSelect = dest.iSdst;
		if (rc || db->mallocFailed || pParse->nErr)
			goto insert_cleanup;
		sqlite3VdbeEndCoroutine(v, regYield);
		sqlite3VdbeJumpHere(v, addrTop - 1);	/* label B: */
		assert(pSelect->pEList);
		nColumn = pSelect->pEList->nExpr;

		/* Set useTempTable to TRUE if the result of the SELECT statement
		 * should be written into a temporary table (template 4).  Set to
		 * FALSE if each output row of the SELECT can be written directly into
		 * the destination table (template 3).
		 *
		 * A temp table must be used if the table being updated is also one
		 * of the tables being read by the SELECT statement.  Also use a
		 * temp table in the case of row triggers.
		 */
		if (pTrigger || readsTable(pParse, pTab)) {
			useTempTable = 1;
		}

		if (useTempTable) {
			/* Invoke the coroutine to extract information from the SELECT
			 * and add it to a transient table srcTab.  The code generated
			 * here is from the 4th template:
			 *
			 *      B: open temp table
			 *      L: yield X, goto M at EOF
			 *         insert row from R..R+n into temp table
			 *         goto L
			 *      M: ...
			 */
			int regRec;	/* Register to hold packed record */
			int regTempId;	/* Register to hold temp table ID */
			int regCopy;    /* Register to keep copy of registers from select */
			int addrL;	/* Label "L" */

			srcTab = pParse->nTab++;
			regRec = sqlite3GetTempReg(pParse);
			regCopy = sqlite3GetTempRange(pParse, nColumn);
			regTempId = sqlite3GetTempReg(pParse);
			KeyInfo *pKeyInfo = sqlite3KeyInfoAlloc(pParse->db, 1+nColumn, 0);
			sqlite3VdbeAddOp4(v, OP_OpenTEphemeral, srcTab, nColumn+1,
					  0, (char*)pKeyInfo, P4_KEYINFO);
			addrL = sqlite3VdbeAddOp1(v, OP_Yield, dest.iSDParm);
			VdbeCoverage(v);
			sqlite3VdbeAddOp3(v, OP_NextIdEphemeral, srcTab, 2, regTempId);
			sqlite3VdbeAddOp3(v, OP_Copy, regFromSelect, regCopy, nColumn-1);
			sqlite3VdbeAddOp3(v, OP_MakeRecord, regCopy,
					  nColumn + 1, regRec);
			/* Set flag to save memory allocating one by malloc. */
			sqlite3VdbeChangeP5(v, 1);
			sqlite3VdbeAddOp2(v, OP_IdxInsert, srcTab, regRec);

			sqlite3VdbeGoto(v, addrL);
			sqlite3VdbeJumpHere(v, addrL);
			sqlite3ReleaseTempReg(pParse, regRec);
			sqlite3ReleaseTempReg(pParse, regTempId);
			sqlite3ReleaseTempRange(pParse, regCopy, nColumn);
		}
	} else {
		/* This is the case if the data for the INSERT is coming from a
		 * single-row VALUES clause
		 */
		NameContext sNC;
		memset(&sNC, 0, sizeof(sNC));
		sNC.pParse = pParse;
		srcTab = -1;
		assert(useTempTable == 0);
		if (pList) {
			nColumn = pList->nExpr;
			if (sqlite3ResolveExprListNames(&sNC, pList)) {
				goto insert_cleanup;
			}
		} else {
			nColumn = 0;
		}
	}

	/* If there is no IDLIST term but the table has an integer primary
	 * key, the set the ipkColumn variable to the integer primary key
	 * column index in the original table definition.
	 */
	if (pColumn == 0 && nColumn > 0) {
		ipkColumn = pTab->iPKey;
	}

	if (pColumn == 0 && nColumn && nColumn != (pTab->nCol)) {
		sqlite3ErrorMsg(pParse,
				"table %S has %d columns but %d values were supplied",
				pTabList, 0, pTab->nCol, nColumn);
		goto insert_cleanup;
	}
	if (pColumn != 0 && nColumn != pColumn->nId) {
		sqlite3ErrorMsg(pParse, "%d values for %d columns", nColumn,
				pColumn->nId);
		goto insert_cleanup;
	}

	/* Initialize the count of rows to be inserted
	 */
	if (user_session->sql_flags & SQLITE_CountRows) {
		regRowCount = ++pParse->nMem;
		sqlite3VdbeAddOp2(v, OP_Integer, 0, regRowCount);
	}

	/* If this is not a view, open the table and and all indices */
	if (!isView) {
		int nIdx;
		nIdx =
		    sqlite3OpenTableAndIndices(pParse, pTab, OP_OpenWrite, 0,
					       -1, 0, &iDataCur, &iIdxCur,
					       onError, 0);

		aRegIdx = sqlite3DbMallocRawNN(db, sizeof(int) * (nIdx + 1));
		if (aRegIdx == 0) {
			goto insert_cleanup;
		}
		for (i = 0, pIdx = pTab->pIndex; i < nIdx;
		     pIdx = pIdx->pNext, i++) {
			assert(pIdx);
			aRegIdx[i] = ++pParse->nMem;
			pParse->nMem += pIdx->nColumn;
		}
	}

	/* This is the top of the main insertion loop */
	if (useTempTable) {
		/* This block codes the top of loop only.  The complete loop is the
		 * following pseudocode (template 4):
		 *
		 *         rewind temp table, if empty goto D
		 *      C: loop over rows of intermediate table
		 *           transfer values form intermediate table into <table>
		 *         end loop
		 *      D: ...
		 */
		addrInsTop = sqlite3VdbeAddOp1(v, OP_Rewind, srcTab);
		VdbeCoverage(v);
		addrCont = sqlite3VdbeCurrentAddr(v);
	} else if (pSelect) {
		/* This block codes the top of loop only.  The complete loop is the
		 * following pseudocode (template 3):
		 *
		 *      C: yield X, at EOF goto D
		 *         insert the select result into <table> from R..R+n
		 *         goto C
		 *      D: ...
		 */
		addrInsTop = addrCont =
		    sqlite3VdbeAddOp1(v, OP_Yield, dest.iSDParm);
		VdbeCoverage(v);
	}

	/* Run the BEFORE and INSTEAD OF triggers, if there are any
	 */
	endOfLoop = sqlite3VdbeMakeLabel(v);
	if (tmask & TRIGGER_BEFORE) {
		int regCols = sqlite3GetTempRange(pParse, pTab->nCol + 1);

		/* Create the new column data
		 */
		for (i = j = 0; i < pTab->nCol; i++) {
			if (pColumn) {
				for (j = 0; j < pColumn->nId; j++) {
					if (pColumn->a[j].idx == i)
						break;
				}
			}
			if ((!useTempTable && !pList)
			    || (pColumn && j >= pColumn->nId)) {
				if (i == pTab->iAutoIncPKey)
					sqlite3VdbeAddOp2(v, OP_Integer, -1,
							  regCols + i + 1);
				else
					sqlite3ExprCode(pParse,
							pTab->aCol[i].pDflt,
							regCols + i + 1);
			} else if (useTempTable) {
				sqlite3VdbeAddOp3(v, OP_Column, srcTab, j,
						  regCols + i + 1);
			} else {
				assert(pSelect == 0);	/* Otherwise useTempTable is true */
				sqlite3ExprCodeAndCache(pParse,
							pList->a[j].pExpr,
							regCols + i + 1);
			}
			if (pColumn == 0)
				j++;
		}

		/* If this is an INSERT on a view with an INSTEAD OF INSERT trigger,
		 * do not attempt any conversions before assembling the record.
		 * If this is a real table, attempt conversions as required by the
		 * table column affinities.
		 */
		if (!isView) {
			sqlite3TableAffinity(v, pTab, regCols + 1);
		}

		/* Fire BEFORE or INSTEAD OF triggers */
		sqlite3CodeRowTrigger(pParse, pTrigger, TK_INSERT, 0,
				      TRIGGER_BEFORE, pTab,
				      regCols - pTab->nCol - 1, onError,
				      endOfLoop);

		sqlite3ReleaseTempRange(pParse, regCols, pTab->nCol + 1);
	}

	/* Compute the content of the next row to insert into a range of
	 * registers beginning at regIns.
	 */
	if (!isView) {
		if (ipkColumn >= 0) {
			if (useTempTable) {
				sqlite3VdbeAddOp3(v, OP_Column, srcTab,
						  ipkColumn, regTupleid);
			} else if (pSelect) {
				sqlite3VdbeAddOp2(v, OP_Copy,
						  regFromSelect + ipkColumn,
						  regTupleid);
			}
		} else {
			sqlite3VdbeAddOp2(v, OP_Null, 0, regTupleid);
		}

		/* Compute data for all columns of the new entry, beginning
		 * with the first column.
		 */
		for (i = 0; i < pTab->nCol; i++) {
			int iRegStore = regTupleid + 1 + i;
			if (pColumn == 0) {
				j = i;
			} else {
				for (j = 0; j < pColumn->nId; j++) {
					if (pColumn->a[j].idx == i)
						break;
				}
			}
			if (j < 0 || nColumn == 0
			    || (pColumn && j >= pColumn->nId)) {
				if (i == pTab->iAutoIncPKey) {
					sqlite3VdbeAddOp2(v, OP_Null, 0, iRegStore);
					continue;
				}
				sqlite3ExprCodeFactorable(pParse,
							  pTab->aCol[i].pDflt,
							  iRegStore);
			} else if (useTempTable) {
				if ((pTab->tabFlags & TF_Autoincrement)
				    && (i == pTab->iAutoIncPKey)) {
					int regTmp = ++pParse->nMem;
					/* Emit code which doesn't override
					 * autoinc-ed value with select result
					 * in case if result is NULL value.
					 */
					sqlite3VdbeAddOp3(v, OP_Column, srcTab,
							  j, regTmp);
					sqlite3VdbeAddOp2(v, OP_FCopy, regTmp,
							  iRegStore);
					sqlite3VdbeChangeP3(v, -1,
							    OPFLAG_SAME_FRAME |
							    OPFLAG_NOOP_IF_NULL);
				} else {
					sqlite3VdbeAddOp3(v, OP_Column, srcTab,
							  j, iRegStore);
				}
			} else if (pSelect) {
				if (regFromSelect != regData) {
					if ((pTab->tabFlags & TF_Autoincrement)
					    && (i == pTab->iAutoIncPKey)) {
						/* Emit code which doesn't override
						 * autoinc-ed value with select result
						 * in case that result is NULL
						 */
						sqlite3VdbeAddOp2(v, OP_FCopy,
								  regFromSelect
								  + j,
								  iRegStore);
						sqlite3VdbeChangeP3(v, -1,
								    OPFLAG_SAME_FRAME
								    |
								    OPFLAG_NOOP_IF_NULL);
					} else {
						sqlite3VdbeAddOp2(v, OP_SCopy,
								  regFromSelect
								  + j,
								  iRegStore);
					}
				}
			} else {

				if (i == pTab->iAutoIncPKey) {
					if (pList->a[j].pExpr->op == TK_NULL) {
						sqlite3VdbeAddOp2(v, OP_Null, 0, iRegStore);
						continue;
					}

					if (pList->a[j].pExpr->op ==
					    TK_REGISTER) {
						/* Emit code which doesn't override
						 * autoinc-ed value with select result
						 * in case that result is NULL
						 */
						sqlite3VdbeAddOp2(v, OP_FCopy,
								  pList->a[j].
								  pExpr->iTable,
								  iRegStore);
						sqlite3VdbeChangeP3(v, -1,
								    OPFLAG_SAME_FRAME
								    |
								    OPFLAG_NOOP_IF_NULL);
						continue;
					}
				}

				sqlite3ExprCode(pParse, pList->a[j].pExpr,
						iRegStore);
			}
		}

		/* Generate code to check constraints and generate index keys
		   and do the insertion.
		 */
		int isReplace;	/* Set to true if constraints may cause a replace */
		int bUseSeek;	/* True to use OPFLAG_SEEKRESULT */
		sqlite3GenerateConstraintChecks(pParse, pTab, aRegIdx, iDataCur,
						iIdxCur, regIns, 0,
						ipkColumn >= 0, onError,
						endOfLoop, &isReplace, 0);
		sqlite3FkCheck(pParse, pTab, 0, regIns, 0);

		/* Set the OPFLAG_USESEEKRESULT flag if either (a) there are no REPLACE
		 * constraints or (b) there are no triggers and this table is not a
		 * parent table in a foreign key constraint. It is safe to set the
		 * flag in the second case as if any REPLACE constraint is hit, an
		 * OP_Delete or OP_IdxDelete instruction will be executed on each
		 * cursor that is disturbed. And these instructions both clear the
		 * VdbeCursor.seekResult variable, disabling the OPFLAG_USESEEKRESULT
		 * functionality.
		 */
		bUseSeek = isReplace == 0 || (pTrigger == 0 &&
					      ((user_session->sql_flags &
						SQLITE_ForeignKeys) == 0 ||
					       sqlite3FkReferences(pTab) == 0));
		sqlite3CompleteInsertion(pParse, pTab, iIdxCur, aRegIdx,
					 bUseSeek, onError);
	}

	/* Update the count of rows that are inserted
	 */
	if ((user_session->sql_flags & SQLITE_CountRows) != 0) {
		sqlite3VdbeAddOp2(v, OP_AddImm, regRowCount, 1);
	}

	if (pTrigger) {
		/* Code AFTER triggers */
		sqlite3CodeRowTrigger(pParse, pTrigger, TK_INSERT, 0,
				      TRIGGER_AFTER, pTab,
				      regData - 2 - pTab->nCol, onError,
				      endOfLoop);
	}

	/* The bottom of the main insertion loop, if the data source
	 * is a SELECT statement.
	 */
	sqlite3VdbeResolveLabel(v, endOfLoop);
	if (useTempTable) {
		sqlite3VdbeAddOp2(v, OP_Next, srcTab, addrCont);
		VdbeCoverage(v);
		sqlite3VdbeJumpHere(v, addrInsTop);
		sqlite3VdbeAddOp1(v, OP_Close, srcTab);
	} else if (pSelect) {
		sqlite3VdbeGoto(v, addrCont);
		sqlite3VdbeJumpHere(v, addrInsTop);
	}

 insert_end:

	/*
	 * Return the number of rows inserted. If this routine is
	 * generating code because of a call to sqlite3NestedParse(), do not
	 * invoke the callback function.
	 */
	if ((user_session->sql_flags & SQLITE_CountRows) && !pParse->nested
	    && !pParse->pTriggerTab) {
		sqlite3VdbeAddOp2(v, OP_ResultRow, regRowCount, 1);
		sqlite3VdbeSetNumCols(v, 1);
		sqlite3VdbeSetColName(v, 0, COLNAME_NAME, "rows inserted",
				      SQLITE_STATIC);
	}

 insert_cleanup:
	sqlite3SrcListDelete(db, pTabList);
	sqlite3ExprListDelete(db, pList);
	sqlite3SelectDelete(db, pSelect);
	sqlite3IdListDelete(db, pColumn);
	sqlite3DbFree(db, aRegIdx);
}

/* Make sure "isView" and other macros defined above are undefined. Otherwise
 * they may interfere with compilation of other functions in this file
 * (or in another file, if this file becomes part of the amalgamation).
 */
#ifdef isView
#undef isView
#endif
#ifdef pTrigger
#undef pTrigger
#endif
#ifdef tmask
#undef tmask
#endif

/*
 * Meanings of bits in of pWalker->eCode for checkConstraintUnchanged()
 */
#define CKCNSTRNT_COLUMN   0x01	/* CHECK constraint uses a changing column */

/* This is the Walker callback from checkConstraintUnchanged().  Set
 * bit 0x01 of pWalker->eCode if
 * pWalker->eCode to 0 if this expression node references any of the
 * columns that are being modifed by an UPDATE statement.
 */
static int
checkConstraintExprNode(Walker * pWalker, Expr * pExpr)
{
	if (pExpr->op == TK_COLUMN) {
		assert(pExpr->iColumn >= 0 || pExpr->iColumn == -1);
		if (pExpr->iColumn >= 0) {
			if (pWalker->u.aiCol[pExpr->iColumn] >= 0) {
				pWalker->eCode |= CKCNSTRNT_COLUMN;
			}
		}
	}
	return WRC_Continue;
}

/*
 * pExpr is a CHECK constraint on a row that is being UPDATE-ed.  The
 * only columns that are modified by the UPDATE are those for which
 * aiChng[i]>=0.
 *
 * Return true if CHECK constraint pExpr does not use any of the
 * changing columns.  In other words, return true if this CHECK constraint
 * can be skipped when validating the new row in the UPDATE statement.
 */
static int
checkConstraintUnchanged(Expr * pExpr, int *aiChng)
{
	Walker w;
	memset(&w, 0, sizeof(w));
	w.eCode = 0;
	w.xExprCallback = checkConstraintExprNode;
	w.u.aiCol = aiChng;
	sqlite3WalkExpr(&w, pExpr);
	testcase(w.eCode == 0);
	testcase(w.eCode == CKCNSTRNT_COLUMN);
	return !w.eCode;
}

/*
 * Generate code to do constraint checks prior to an INSERT or an UPDATE
 * on table pTab.
 *
 * The regNewData parameter is the first register in a range that contains
 * the data to be inserted or the data after the update.  There will be
 * pTab->nCol+1 registers in this range.  The first register (the one
 * that regNewData points to) will contain NULL.  The second register
 * in the range will contain the content of the first table column.
 * The third register will contain the content of the second table column.
 * And so forth.
 *
 * The regOldData parameter is similar to regNewData except that it contains
 * the data prior to an UPDATE rather than afterwards.  regOldData is zero
 * for an INSERT.  This routine can distinguish between UPDATE and INSERT by
 * checking regOldData for zero.
 *
 * For an UPDATE, the pkChng boolean is true if the primary key
 * might be modified by the UPDATE.  If pkChng is false, then the key of
 * the iDataCur content table is guaranteed to be unchanged by the UPDATE.
 *
 * On an INSERT, pkChng will only be true if the INSERT statement provides
 * an integer value for INTEGER PRIMARY KEY alias.
 *
 * The code generated by this routine will store new index entries into
 * registers identified by aRegIdx[].  No index entry is created for
 * indices where aRegIdx[i]==0.  The order of indices in aRegIdx[] is
 * the same as the order of indices on the linked list of indices
 * at pTab->pIndex.
 *
 * The caller must have already opened writeable cursors on the main
 * table and all applicable indices (that is to say, all indices for which
 * aRegIdx[] is not zero).  iDataCur is the cursor for the PRIMARY KEY index.
 * iIdxCur is the cursor for the first index in the pTab->pIndex list.
 * Cursors for other indices are at iIdxCur+N for the N-th element
 * of the pTab->pIndex list.
 *
 * This routine also generates code to check constraints.  NOT NULL,
 * CHECK, and UNIQUE constraints are all checked.  If a constraint fails,
 * then the appropriate action is performed.  There are five possible
 * actions: ROLLBACK, ABORT, FAIL, REPLACE, and IGNORE.
 *
 *  Constraint type  Action       What Happens
 *  ---------------  ----------   ----------------------------------------
 *  any              ROLLBACK     The current transaction is rolled back and
 *                                sqlite3_step() returns immediately with a
 *                                return code of SQLITE_CONSTRAINT.
 *
 *  any              ABORT        Back out changes from the current command
 *                                only (do not do a complete rollback) then
 *                                cause sqlite3_step() to return immediately
 *                                with SQLITE_CONSTRAINT.
 *
 *  any              FAIL         Sqlite3_step() returns immediately with a
 *                                return code of SQLITE_CONSTRAINT.  The
 *                                transaction is not rolled back and any
 *                                changes to prior rows are retained.
 *
 *  any              IGNORE       The attempt in insert or update the current
 *                                row is skipped, without throwing an error.
 *                                Processing continues with the next row.
 *                                (There is an immediate jump to ignoreDest.)
 *
 *  NOT NULL         REPLACE      The NULL value is replace by the default
 *                                value for that column.  If the default value
 *                                is NULL, the action is the same as ABORT.
 *
 *  UNIQUE           REPLACE      The other row that conflicts with the row
 *                                being inserted is removed.
 *
 *  CHECK            REPLACE      Illegal.  The results in an exception.
 *
 * Which action to take is determined by the overrideError parameter.
 * Or if overrideError==ON_CONFLICT_ACTION_DEFAULT, then the pParse->onError parameter
 * is used.  Or if pParse->onError==ON_CONFLICT_ACTION_DEFAULT then the onError value
 * for the constraint is used.
 */
void
sqlite3GenerateConstraintChecks(Parse * pParse,		/* The parser context */
				Table * pTab,		/* The table being inserted or updated */
				int *aRegIdx,		/* Use register aRegIdx[i] for index i.  0 for unused */
				int iDataCur,		/* Canonical data cursor (main table or PK index) */
				int iIdxCur,		/* First index cursor */
				int regNewData,		/* First register in a range holding values to insert */
				int regOldData,		/* Previous content.  0 for INSERTs */
				u8 pkChng,		/* Non-zero if the PRIMARY KEY changed */
				u8 overrideError,	/* Override onError to this if not ON_CONFLICT_ACTION_DEFAULT */
				int ignoreDest,		/* Jump to this label on an ON_CONFLICT_ACTION_IGNORE resolution */
				int *pbMayReplace,	/* OUT: Set to true if constraint may cause a replace */
				int *aiChng)		/* column i is unchanged if aiChng[i]<0 */
{
	Vdbe *v;		/* VDBE under constrution */
	Index *pIdx;		/* Pointer to one of the indices */
	Index *pPk = 0;		/* The PRIMARY KEY index */
	sqlite3 *db;		/* Database connection */
	int i;			/* loop counter */
	int ix;			/* Index loop counter */
	int nCol;		/* Number of columns */
	int onError;		/* Conflict resolution strategy */
	int addr1;		/* Address of jump instruction */
	int seenReplace = 0;	/* True if REPLACE is used to resolve INT PK conflict */
	int nPkField;		/* Number of fields in PRIMARY KEY. */
	u8 isUpdate;		/* True if this is an UPDATE operation */
	u8 bAffinityDone = 0;	/* True if the OP_Affinity operation has been run */
	struct session *user_session = current_session();

	isUpdate = regOldData != 0;
	db = pParse->db;
	v = sqlite3GetVdbe(pParse);
	assert(v != 0);
	assert(pTab->pSelect == 0);	/* This table is not a VIEW */
	nCol = pTab->nCol;

	pPk = sqlite3PrimaryKeyIndex(pTab);
	nPkField = pPk->nKeyCol;

	/* Record that this module has started */
	VdbeModuleComment((v, "BEGIN: GenCnstCks(%d,%d,%d,%d,%d)",
			   iDataCur, iIdxCur, regNewData, regOldData, pkChng));

	/* Test all NOT NULL constraints.
	 */
	for (i = 0; i < nCol; i++) {
		if (i == pTab->iPKey) {
			continue;
		}
		if (aiChng && aiChng[i] < 0) {
			/* Don't bother checking for NOT NULL on columns that do not change */
			continue;
		}
		if (table_column_is_nullable(pTab, i)
		    || (pTab->tabFlags & TF_Autoincrement
			&& pTab->iAutoIncPKey == i))
			continue;	/* This column is allowed to be NULL */

		onError = table_column_nullable_action(pTab, i);
		if (overrideError != ON_CONFLICT_ACTION_DEFAULT) {
			onError = overrideError;
		} else if (onError == ON_CONFLICT_ACTION_DEFAULT) {
			onError = ON_CONFLICT_ACTION_ABORT;
		}
		if (onError == ON_CONFLICT_ACTION_REPLACE
		    && pTab->aCol[i].pDflt == 0) {
			onError = ON_CONFLICT_ACTION_ABORT;
		}
		assert(onError == ON_CONFLICT_ACTION_ROLLBACK
		       || onError == ON_CONFLICT_ACTION_ABORT
		       || onError == ON_CONFLICT_ACTION_FAIL
		       || onError == ON_CONFLICT_ACTION_IGNORE
		       || onError == ON_CONFLICT_ACTION_REPLACE);
		switch (onError) {
		case ON_CONFLICT_ACTION_ABORT:
			sqlite3MayAbort(pParse);
			/* Fall through */
		case ON_CONFLICT_ACTION_ROLLBACK:
		case ON_CONFLICT_ACTION_FAIL: {
				char *zMsg =
				    sqlite3MPrintf(db, "%s.%s", pTab->zName,
						   pTab->aCol[i].zName);
				sqlite3VdbeAddOp3(v, OP_HaltIfNull,
						  SQLITE_CONSTRAINT_NOTNULL,
						  onError, regNewData + 1 + i);
				sqlite3VdbeAppendP4(v, zMsg, P4_DYNAMIC);
				sqlite3VdbeChangeP5(v, P5_ConstraintNotNull);
				VdbeCoverage(v);
				break;
			}
		case ON_CONFLICT_ACTION_IGNORE: {
				sqlite3VdbeAddOp2(v, OP_IsNull,
						  regNewData + 1 + i,
						  ignoreDest);
				VdbeCoverage(v);
				break;
			}
		default:{
				assert(onError == ON_CONFLICT_ACTION_REPLACE);
				addr1 =
				    sqlite3VdbeAddOp1(v, OP_NotNull,
						      regNewData + 1 + i);
				VdbeCoverage(v);
				sqlite3ExprCode(pParse, pTab->aCol[i].pDflt,
						regNewData + 1 + i);
				sqlite3VdbeJumpHere(v, addr1);
				break;
			}
		}
	}

	/* Test all CHECK constraints
	 */
#ifndef SQLITE_OMIT_CHECK
	if (pTab->pCheck && (user_session->sql_flags &
			     SQLITE_IgnoreChecks) == 0) {
		ExprList *pCheck = pTab->pCheck;
		pParse->ckBase = regNewData + 1;
		onError =
		    overrideError != ON_CONFLICT_ACTION_DEFAULT ? overrideError
			: ON_CONFLICT_ACTION_ABORT;
		for (i = 0; i < pCheck->nExpr; i++) {
			int allOk;
			Expr *pExpr = pCheck->a[i].pExpr;
			if (aiChng
			    && checkConstraintUnchanged(pExpr, aiChng))
				continue;
			allOk = sqlite3VdbeMakeLabel(v);
			sqlite3ExprIfTrue(pParse, pExpr, allOk,
					  SQLITE_JUMPIFNULL);
			if (onError == ON_CONFLICT_ACTION_IGNORE) {
				sqlite3VdbeGoto(v, ignoreDest);
			} else {
				char *zName = pCheck->a[i].zName;
				if (zName == 0)
					zName = pTab->zName;
				if (onError == ON_CONFLICT_ACTION_REPLACE)
					onError = ON_CONFLICT_ACTION_ABORT;
				sqlite3HaltConstraint(pParse,
						      SQLITE_CONSTRAINT_CHECK,
						      onError, zName,
						      P4_TRANSIENT,
						      P5_ConstraintCheck);
			}
			sqlite3VdbeResolveLabel(v, allOk);
		}
	}
#endif				/* !defined(SQLITE_OMIT_CHECK) */

	/* Test all UNIQUE constraints by creating entries for each UNIQUE
	 * index and making sure that duplicate entries do not already exist.
	 * Compute the revised record entries for indices as we go.
	 *
	 * This loop also handles the case of the PRIMARY KEY index.
	 */
	for (ix = 0, pIdx = pTab->pIndex; pIdx; pIdx = pIdx->pNext, ix++) {
		int regIdx;	/* Range of registers hold conent for pIdx */
		int regR;	/* Range of registers holding conflicting PK */
		int iThisCur;	/* Cursor for this UNIQUE index */
		int addrUniqueOk;	/* Jump here if the UNIQUE constraint is satisfied */
		bool uniqueByteCodeNeeded = false;

		if ((pIdx->onError != ON_CONFLICT_ACTION_ABORT &&
		     pIdx->onError != ON_CONFLICT_ACTION_DEFAULT) ||
		    (overrideError != ON_CONFLICT_ACTION_ABORT &&
		     overrideError != ON_CONFLICT_ACTION_DEFAULT)) {
			uniqueByteCodeNeeded = true;
		}

		if (aRegIdx[ix] == 0)
			continue;	/* Skip indices that do not change */
		if (bAffinityDone == 0) {
			sqlite3TableAffinity(v, pTab, regNewData+1);
			bAffinityDone = 1;
		}
		iThisCur = iIdxCur + ix;
		addrUniqueOk = sqlite3VdbeMakeLabel(v);

		/* Skip partial indices for which the WHERE clause is not true */
		if (pIdx->pPartIdxWhere) {
			sqlite3VdbeAddOp2(v, OP_Null, 0, aRegIdx[ix]);
			pParse->ckBase = regNewData + 1;
			sqlite3ExprIfFalseDup(pParse, pIdx->pPartIdxWhere,
					      addrUniqueOk, SQLITE_JUMPIFNULL);
			pParse->ckBase = 0;
		}

		/* Create a record for this index entry as it should appear after
		 * the insert or update.  Store that record in the aRegIdx[ix] register
		 */
		regIdx = aRegIdx[ix] + 1;
		for (i = 0; i < pIdx->nColumn; i++) {
			int iField = pIdx->aiColumn[i];
			int x;
			if (iField == XN_EXPR) {
				pParse->ckBase = regNewData + 1;
				sqlite3ExprCodeCopy(pParse,
						    pIdx->aColExpr->a[i].pExpr,
						    regIdx + i);
				pParse->ckBase = 0;
				VdbeComment((v, "%s column %d", pIdx->zName,
					     i));
			} else {
				if (iField == pTab->iPKey) {
					x = regNewData;
				} else {
					x = iField + regNewData + 1;
				}
				assert(iField >= 0);
				sqlite3VdbeAddOp2(v, OP_SCopy, x, regIdx + i);
				VdbeComment((v, "%s", pTab->aCol[iField].zName));
			}
		}

		bool table_ipk_autoinc = false;
		int reg_pk = -1;
		if (IsPrimaryKeyIndex(pIdx)) {
			/* If PK is marked as INTEGER, use it as strict type,
			 * not as affinity. Emit code for type checking */
			if (pIdx->nKeyCol == 1) {
				reg_pk = regNewData + 1 + pIdx->aiColumn[0];
				if (pTab->zColAff[pIdx->aiColumn[0]] == 'D') {
					int skip_if_null = sqlite3VdbeMakeLabel(v);
					if ((pTab->tabFlags & TF_Autoincrement) != 0) {
						sqlite3VdbeAddOp2(v, OP_IsNull,
								  reg_pk,
								  skip_if_null);
						table_ipk_autoinc = true;
					}
					sqlite3VdbeAddOp2(v, OP_MustBeInt,
							  reg_pk,
							  0);
					sqlite3VdbeResolveLabel(v, skip_if_null);
				}
			}
			if (IsPrimaryKeyIndex(pIdx) || uniqueByteCodeNeeded) {
				sqlite3VdbeAddOp3(v, OP_MakeRecord, regNewData + 1,
						  pTab->nCol, aRegIdx[ix]);
				VdbeComment((v, "for %s", pIdx->zName));
			}
		} else {
			/* kyukhin: for Tarantool, this should be evaluated to NOP.  */
			if (IsPrimaryKeyIndex(pIdx) || uniqueByteCodeNeeded) {
				sqlite3VdbeAddOp3(v, OP_MakeRecord, regIdx,
						  pIdx->nColumn, aRegIdx[ix]);
				VdbeComment((v, "for %s", pIdx->zName));
			}
		}

		/* In an UPDATE operation, if this index is the PRIMARY KEY
		 * index and there has been no change the primary key, then no
		 * collision is possible.  The collision detection
		 * logic below can all be skipped.
		 */
		if (isUpdate && pPk == pIdx && pkChng == 0) {
			sqlite3VdbeResolveLabel(v, addrUniqueOk);
			continue;
		}

		/* Find out what action to take in case there is a uniqueness conflict */
		onError = pIdx->onError;
		if (onError == ON_CONFLICT_ACTION_NONE) {
			sqlite3VdbeResolveLabel(v, addrUniqueOk);
			continue;	/* pIdx is not a UNIQUE index */
		}
		/* If pIdx is not a UNIQUE or we are doing INSERT OR IGNORE,
		 * INSERT OR FAIL then skip uniqueness checks and let it to be
		 * done by Tarantool.
		 */
		if (overrideError == ON_CONFLICT_ACTION_FAIL ||
		    overrideError == ON_CONFLICT_ACTION_IGNORE ||
		    overrideError == ON_CONFLICT_ACTION_ABORT) {
			sqlite3VdbeResolveLabel(v, addrUniqueOk);
			continue;	/* pIdx is not a UNIQUE index */
		}
		if (overrideError != ON_CONFLICT_ACTION_DEFAULT) {
			onError = overrideError;
		} else if (onError == ON_CONFLICT_ACTION_DEFAULT) {
			onError = ON_CONFLICT_ACTION_ABORT;
		}

		/* Collision detection may be omitted if all of the following are true:
		 *   (1) The conflict resolution algorithm is REPLACE
		 *   (2) There are no secondary indexes on the table
		 *   (3) No delete triggers need to be fired if there is a conflict
		 *   (4) No FK constraint counters need to be updated if a conflict occurs.
		 */
		if ((ix == 0 && pIdx->pNext == 0)	/* Condition 2 */
		    && onError == ON_CONFLICT_ACTION_REPLACE	/* Condition 1 */
		    && (0 == (user_session->sql_flags & SQLITE_RecTriggers)	/* Condition 3 */
			||0 == sqlite3TriggersExist(pTab, TK_DELETE, 0, 0))
		    && (0 == (user_session->sql_flags & SQLITE_ForeignKeys) ||	/* Condition 4 */
			(0 == pTab->pFKey && 0 == sqlite3FkReferences(pTab)))
		    ) {
			sqlite3VdbeResolveLabel(v, addrUniqueOk);
			continue;
		}

		/* Check to see if the new index entry will be unique */
		if (table_ipk_autoinc)
			sqlite3VdbeAddOp2(v, OP_IsNull,
					  reg_pk,
					  addrUniqueOk);

		if (uniqueByteCodeNeeded) {
			sqlite3VdbeAddOp4Int(v, OP_NoConflict, iThisCur, addrUniqueOk,
					     regIdx, pIdx->nKeyCol);
		}
		VdbeCoverage(v);

		/* Generate code to handle collisions */
		regR =
		    (pIdx == pPk) ? regIdx : sqlite3GetTempRange(pParse,
								 nPkField);
		if (isUpdate || onError == ON_CONFLICT_ACTION_REPLACE) {
			int x;
			/* Extract the PRIMARY KEY from the end of the index entry and
			 * store it in registers regR..regR+nPk-1
			 */
			if (pIdx != pPk) {
				for (i = 0; i < pPk->nKeyCol; i++) {
					assert(pPk->aiColumn[i] >= 0);
					x = sqlite3ColumnOfIndex(pIdx,
								 pPk->aiColumn[i]);
					sqlite3VdbeAddOp3(v, OP_Column,
							  iThisCur, x, regR + i);
					VdbeComment((v, "%s.%s", pTab->zName,
						pTab->aCol[pPk->aiColumn[i]].zName));
				}
			}
			if (isUpdate && uniqueByteCodeNeeded) {
				/* Only conflict if the new PRIMARY KEY
				 * values are actually different from the old.
				 *
				 * For a UNIQUE index, only conflict if the PRIMARY KEY values
				 * of the matched index row are different from the original PRIMARY
				 * KEY values of this row before the update.
				 */
				int addrJump =
					sqlite3VdbeCurrentAddr(v) + pPk->nKeyCol;
				int op = OP_Ne;
				int regCmp = (IsPrimaryKeyIndex(pIdx) ?
					      regIdx : regR);

				for (i = 0; i < pPk->nKeyCol; i++) {
					char *p4 = (char *)
						sqlite3LocateCollSeq(pParse, db,
								     pPk->azColl[i]);
					x = pPk->aiColumn[i];
					assert(x >= 0);
					if (i == (pPk->nKeyCol - 1)) {
						addrJump = addrUniqueOk;
						op = OP_Eq;
					}
					sqlite3VdbeAddOp4(v, op, regOldData + 1 + x,
							  addrJump, regCmp + i,
							  p4, P4_COLLSEQ);
					sqlite3VdbeChangeP5(v, SQLITE_NOTNULL);
					VdbeCoverageIf(v, op == OP_Eq);
					VdbeCoverageIf(v, op == OP_Ne);
				}
			}
		}

		/* Generate code that executes if the new index entry is not unique */
		assert(onError == ON_CONFLICT_ACTION_ROLLBACK
		       || onError == ON_CONFLICT_ACTION_ABORT
		       || onError == ON_CONFLICT_ACTION_FAIL
		       || onError == ON_CONFLICT_ACTION_IGNORE
		       || onError == ON_CONFLICT_ACTION_REPLACE);
		switch (onError) {
		case ON_CONFLICT_ACTION_FAIL:
		case ON_CONFLICT_ACTION_ROLLBACK: {
				sqlite3UniqueConstraint(pParse, onError, pIdx);
				break;
			}
		case ON_CONFLICT_ACTION_ABORT: {
				break;
			}
		case ON_CONFLICT_ACTION_IGNORE: {
				sqlite3VdbeGoto(v, ignoreDest);
				break;
			}
		default: {
				Trigger *pTrigger = 0;
				assert(onError == ON_CONFLICT_ACTION_REPLACE);
				sqlite3MultiWrite(pParse);
				if (user_session->
				    sql_flags & SQLITE_RecTriggers) {
					pTrigger =
					    sqlite3TriggersExist(pTab,
								 TK_DELETE, 0,
								 0);
				}
				sqlite3GenerateRowDelete(pParse, pTab, pTrigger,
							 iDataCur, iIdxCur,
							 regR, nPkField, 0,
							 ON_CONFLICT_ACTION_REPLACE,
							 (pIdx ==
							  pPk ? ONEPASS_SINGLE :
							  ONEPASS_OFF), -1);
				seenReplace = 1;
				break;
			}
		}
		sqlite3VdbeResolveLabel(v, addrUniqueOk);
		if (regR != regIdx)
			sqlite3ReleaseTempRange(pParse, regR, nPkField);
	}

	*pbMayReplace = seenReplace;
	VdbeModuleComment((v, "END: GenCnstCks(%d)", seenReplace));
}

/*
 * This routine generates code to finish the INSERT or UPDATE operation
 * that was started by a prior call to sqlite3GenerateConstraintChecks.
 * A consecutive range of registers starting at regNewData contains the
 * tupleid and the content to be inserted.
 *
 * The arguments to this routine should be the same as corresponding
 * arguments to sqlite3GenerateConstraintChecks.
 */
void
sqlite3CompleteInsertion(Parse * pParse,	/* The parser context */
			 Table * pTab,		/* the table into which we are inserting */
			 int iIdxCur,		/* Primary index cursor */
			 int *aRegIdx,		/* Register used by each index.  0 for unused indices */
			 int useSeekResult,	/* True to set the USESEEKRESULT flag on OP_[Idx]Insert */
			 u8 onError)
{
	Vdbe *v;		/* Prepared statements under construction */
	Index *pIdx;		/* An index being inserted or updated */
	u16 pik_flags;		/* flag values passed to the btree insert */
	int opcode;

	v = sqlite3GetVdbe(pParse);
	assert(v != 0);
	assert(pTab->pSelect == 0);	/* This table is not a VIEW */
	/*
	 * The for loop which purpose in sqlite was to insert new
	 * values to all indexes is replaced to inserting new
	 * values only to pk in tarantool.
	 */
	pIdx = pTab->pIndex;
	/* Each table have pk on top of the indexes list */
	assert(IsPrimaryKeyIndex(pIdx));
	/* Partial indexes should be implemented somewhere in tarantool
	 * codebase to check it during inserting values to the pk #2626
	 *
	 */
	/*if( pIdx->pPartIdxWhere ){
	 *  sqlite3VdbeAddOp2(v, OP_IsNull, aRegIdx[i], sqlite3VdbeCurrentAddr(v)+2);
	 *  VdbeCoverage(v);
	 *}
	 */
	pik_flags = OPFLAG_NCHANGE;
	if (useSeekResult) {
		pik_flags |= OPFLAG_USESEEKRESULT;
	}
	assert(pParse->nested == 0);

	if (onError == ON_CONFLICT_ACTION_REPLACE) {
		opcode = OP_IdxReplace;
	} else {
		opcode = OP_IdxInsert;
	}

	if (onError == ON_CONFLICT_ACTION_IGNORE) {
		pik_flags |= OPFLAG_OE_IGNORE;
	} else if (onError == ON_CONFLICT_ACTION_FAIL) {
		pik_flags |= OPFLAG_OE_FAIL;
	}

	sqlite3VdbeAddOp4Int(v, opcode, iIdxCur, aRegIdx[0],
			     aRegIdx[0] + 1,
			     pIdx->uniqNotNull ? pIdx->nKeyCol : pIdx->nColumn);
	sqlite3VdbeChangeP5(v, pik_flags);
}

/*
 * Allocate cursors for the pTab table and all its indices and generate
 * code to open and initialized those cursors.
 *
 * The cursor for the object that contains the complete data (index)
 * is returned in *piDataCur.  The first index cursor is
 * returned in *piIdxCur.  The number of indices is returned.
 *
 * Use iBase as the first cursor (the first index) if it is non-negative.
 * If iBase is negative, then allocate the next available cursor.
 *
 * *piDataCur will be somewhere in the range
 * of *piIdxCurs, depending on where the PRIMARY KEY index appears on the
 * pTab->pIndex list.
 */
int
sqlite3OpenTableAndIndices(Parse * pParse,	/* Parsing context */
			   Table * pTab,	/* Table to be opened */
			   int op,		/* OP_OpenRead or OP_OpenWrite */
			   u8 p5,		/* P5 value for OP_Open* opcodes */
			   int iBase,		/* Use this for the table cursor, if there is one */
			   u8 * aToOpen,	/* If not NULL: boolean for each table and index */
			   int *piDataCur,	/* Write the database source cursor number here */
			   int *piIdxCur, 	/* Write the first index cursor number here */
			   u8 overrideError,	/* Override error action for indexes */
			   u8 isUpdate)		/* Opened for udpate or not */
{
	int i;
	int iDataCur;
	Index *pIdx;
	Vdbe *v;

	assert(op == OP_OpenRead || op == OP_OpenWrite);
	assert(op == OP_OpenWrite || p5 == 0);
	v = sqlite3GetVdbe(pParse);
	assert(v != 0);
	if (iBase < 0)
		iBase = pParse->nTab;
	iDataCur = iBase++;
	if (piDataCur)
		*piDataCur = iDataCur;
	if (piIdxCur)
		*piIdxCur = iBase;
	struct space *space = space_by_id(SQLITE_PAGENO_TO_SPACEID(pTab->tnum));
	assert(space != NULL);
	int space_ptr_reg = ++pParse->nMem;
	sqlite3VdbeAddOp4Int64(v, OP_Int64, 0, space_ptr_reg, 0,
			       ((int64_t) space));

	/* One iteration of this cycle adds OpenRead/OpenWrite which
	 * opens cursor for current index.
	 *
	 * For update cursor on index is required, however if insertion
	 * is done by Tarantool only, cursor is not needed so don't
	 * open it.
	 */
	for (i = 0, pIdx = pTab->pIndex; pIdx; pIdx = pIdx->pNext, i++) {
		/* Cursor is needed:
		 * 1) For indexes in UPDATE statement
		 * 2) For PRIMARY KEY index
		 * 3) For table mentioned in FOREIGN KEY constraint
		 * 4) For an index which has ON CONFLICT action which require
		 *    VDBE bytecode - ROLLBACK, IGNORE, FAIL, REPLACE:
		 *    1. If user specified non-default ON CONFLICT (not
		 *       ON_CONFLICT_ACTION_NONE or _Abort) clause
		 *       for an non-primary unique index, then bytecode is needed
		 *    	 for proper error action.
		 *    2. INSERT/UPDATE OR IGNORE/ABORT/FAIL/REPLACE -
		 *       Tarantool is able handle by itself.
		 *       INSERT/UPDATE OR ROLLBACK - sql
		 *       bytecode is needed in this case.
		 *
		 * If all conditions from list above are false then skip
		 * iteration and don't open new index cursor
		 */

		if (isUpdate || 			/* Condition 1 */
		    IsPrimaryKeyIndex(pIdx) ||		/* Condition 2 */
		    sqlite3FkReferences(pTab) ||	/* Condition 3 */
		    /* Condition 4 */
		    (IsUniqueIndex(pIdx) && pIdx->onError !=
		     ON_CONFLICT_ACTION_DEFAULT &&
		     /* Condition 4.1 */
		     pIdx->onError != ON_CONFLICT_ACTION_ABORT) ||
		     /* Condition 4.2 */
		     overrideError == ON_CONFLICT_ACTION_ROLLBACK) {

			int iIdxCur = iBase++;
			assert(pIdx->pSchema == pTab->pSchema);
			if (IsPrimaryKeyIndex(pIdx)) {
				if (piDataCur)
					*piDataCur = iIdxCur;
				p5 = 0;
			}
			if (aToOpen == 0 || aToOpen[i + 1]) {
				sqlite3VdbeAddOp3(v, op, iIdxCur, pIdx->tnum,
						  space_ptr_reg);
				sqlite3VdbeSetP4KeyInfo(pParse, pIdx);
				sqlite3VdbeChangeP5(v, p5);
				VdbeComment((v, "%s", pIdx->zName));
			}
		}
	}
	if (iBase > pParse->nTab)
		pParse->nTab = iBase;
	return i;
}

#ifdef SQLITE_TEST
/*
 * The following global variable is incremented whenever the
 * transfer optimization is used.  This is used for testing
 * purposes only - to make sure the transfer optimization really
 * is happening when it is supposed to.
 */
int sqlite3_xferopt_count;
#endif				/* SQLITE_TEST */

#ifndef SQLITE_OMIT_XFER_OPT
/*
 * Check to see if index pSrc is compatible as a source of data
 * for index pDest in an insert transfer optimization.  The rules
 * for a compatible index:
 *
 *    *   The index is over the same set of columns
 *    *   The same DESC and ASC markings occurs on all columns
 *    *   The same onError processing (ON_CONFLICT_ACTION_ABORT, _IGNORE, etc)
 *    *   The same collating sequence on each column
 *    *   The index has the exact same WHERE clause
 */
static int
xferCompatibleIndex(Index * pDest, Index * pSrc)
{
	int i;
	assert(pDest && pSrc);
	assert(pDest->pTable != pSrc->pTable);
	if (pDest->nKeyCol != pSrc->nKeyCol) {
		return 0;	/* Different number of columns */
	}
	if (pDest->onError != pSrc->onError) {
		return 0;	/* Different conflict resolution strategies */
	}
	for (i = 0; i < pSrc->nKeyCol; i++) {
		if (pSrc->aiColumn[i] != pDest->aiColumn[i]) {
			return 0;	/* Different columns indexed */
		}
		if (pSrc->aiColumn[i] == XN_EXPR) {
			assert(pSrc->aColExpr != 0 && pDest->aColExpr != 0);
			if (sqlite3ExprCompare(pSrc->aColExpr->a[i].pExpr,
					       pDest->aColExpr->a[i].pExpr,
					       -1) != 0) {
				return 0;	/* Different expressions in the index */
			}
		}
		if (pSrc->aSortOrder[i] != pDest->aSortOrder[i]) {
			return 0;	/* Different sort orders */
		}
		if (sqlite3_stricmp(pSrc->azColl[i], pDest->azColl[i]) != 0) {
			return 0;	/* Different collating sequences */
		}
	}
	if (sqlite3ExprCompare(pSrc->pPartIdxWhere, pDest->pPartIdxWhere, -1)) {
		return 0;	/* Different WHERE clauses */
	}

	/* If no test above fails then the indices must be compatible */
	return 1;
}

/*
 * Attempt the transfer optimization on INSERTs of the form
 *
 *     INSERT INTO tab1 SELECT * FROM tab2;
 *
 * The xfer optimization transfers raw records from tab2 over to tab1.
 * Columns are not decoded and reassembled, which greatly improves
 * performance.  Raw index records are transferred in the same way.
 *
 * The xfer optimization is only attempted if tab1 and tab2 are compatible.
 * There are lots of rules for determining compatibility - see comments
 * embedded in the code for details.
 *
 * This routine returns TRUE if the optimization is guaranteed to be used.
 * Sometimes the xfer optimization will only work if the destination table
 * is empty - a factor that can only be determined at run-time.  In that
 * case, this routine generates code for the xfer optimization but also
 * does a test to see if the destination table is empty and jumps over the
 * xfer optimization code if the test fails.  In that case, this routine
 * returns FALSE so that the caller will know to go ahead and generate
 * an unoptimized transfer.  This routine also returns FALSE if there
 * is no chance that the xfer optimization can be applied.
 */
static int
xferOptimization(Parse * pParse,	/* Parser context */
		 Table * pDest,		/* The table we are inserting into */
		 Select * pSelect,	/* A SELECT statement to use as the data source */
		 int onError)		/* How to handle constraint errors */
{
	ExprList *pEList;	/* The result set of the SELECT */
	Table *pSrc;		/* The table in the FROM clause of SELECT */
	Index *pSrcIdx, *pDestIdx;	/* Source and destination indices */
	struct SrcList_item *pItem;	/* An element of pSelect->pSrc */
	int i;			/* Loop counter */
	int iSrc, iDest;	/* Cursors from source and destination */
	int addr1;		/* Loop addresses */
	int emptyDestTest = 0;	/* Address of test for empty pDest */
	int emptySrcTest = 0;	/* Address of test for empty pSrc */
	Vdbe *v;		/* The VDBE we are building */
	int destHasUniqueIdx = 0;	/* True if pDest has a UNIQUE index */
	int regData, regTupleid;	/* Registers holding data and tupleid */
	struct session *user_session = current_session();

	if (pSelect == 0) {
		return 0;	/* Must be of the form  INSERT INTO ... SELECT ... */
	}
	if (pParse->pWith || pSelect->pWith) {
		/* Do not attempt to process this query if there are an WITH clauses
		 * attached to it. Proceeding may generate a false "no such table: xxx"
		 * error if pSelect reads from a CTE named "xxx".
		 */
		return 0;
	}
	if (pDest->pTrigger) {
		return 0;	/* tab1 must not have triggers */
	}
	if (onError == ON_CONFLICT_ACTION_DEFAULT) {
		if (pDest->iPKey >= 0)
			onError = pDest->keyConf;
		if (onError == ON_CONFLICT_ACTION_DEFAULT)
			onError = ON_CONFLICT_ACTION_ABORT;
	}
	assert(pSelect->pSrc);	/* allocated even if there is no FROM clause */
	if (pSelect->pSrc->nSrc != 1) {
		return 0;	/* FROM clause must have exactly one term */
	}
	if (pSelect->pSrc->a[0].pSelect) {
		return 0;	/* FROM clause cannot contain a subquery */
	}
	if (pSelect->pWhere) {
		return 0;	/* SELECT may not have a WHERE clause */
	}
	if (pSelect->pOrderBy) {
		return 0;	/* SELECT may not have an ORDER BY clause */
	}
	/* Do not need to test for a HAVING clause.  If HAVING is present but
	 * there is no ORDER BY, we will get an error.
	 */
	if (pSelect->pGroupBy) {
		return 0;	/* SELECT may not have a GROUP BY clause */
	}
	if (pSelect->pLimit) {
		return 0;	/* SELECT may not have a LIMIT clause */
	}
	assert(pSelect->pOffset == 0);	/* Must be so if pLimit==0 */
	if (pSelect->pPrior) {
		return 0;	/* SELECT may not be a compound query */
	}
	if (pSelect->selFlags & SF_Distinct) {
		return 0;	/* SELECT may not be DISTINCT */
	}
	pEList = pSelect->pEList;
	assert(pEList != 0);
	if (pEList->nExpr != 1) {
		return 0;	/* The result set must have exactly one column */
	}
	assert(pEList->a[0].pExpr);
	if (pEList->a[0].pExpr->op != TK_ASTERISK) {
		return 0;	/* The result set must be the special operator "*" */
	}

	/* At this point we have established that the statement is of the
	 * correct syntactic form to participate in this optimization.  Now
	 * we have to check the semantics.
	 */
	pItem = pSelect->pSrc->a;
	pSrc = sqlite3LocateTableItem(pParse, 0, pItem);
	if (pSrc == 0) {
		return 0;	/* FROM clause does not contain a real table */
	}
	if (pSrc == pDest) {
		return 0;	/* tab1 and tab2 may not be the same table */
	}
	if (pSrc->pSelect) {
		return 0;	/* tab2 may not be a view */
	}
	if (pDest->nCol != pSrc->nCol) {
		return 0;	/* Number of columns must be the same in tab1 and tab2 */
	}
	if (pDest->iPKey != pSrc->iPKey) {
		return 0;	/* Both tables must have the same INTEGER PRIMARY KEY */
	}
	for (i = 0; i < pDest->nCol; i++) {
		Column *pDestCol = &pDest->aCol[i];
		Column *pSrcCol = &pSrc->aCol[i];
		if (pDestCol->affinity != pSrcCol->affinity) {
			return 0;	/* Affinity must be the same on all columns */
		}
		if (sqlite3_stricmp(pDestCol->zColl, pSrcCol->zColl) != 0) {
			return 0;	/* Collating sequence must be the same on all columns */
		}
		if (!table_column_is_nullable(pDest, i)
		    && table_column_is_nullable(pSrc, i)) {
			return 0;	/* tab2 must be NOT NULL if tab1 is */
		}
		/* Default values for second and subsequent columns need to match. */
		if (i > 0) {
			assert(pDestCol->pDflt == 0
			       || pDestCol->pDflt->op == TK_SPAN);
			assert(pSrcCol->pDflt == 0
			       || pSrcCol->pDflt->op == TK_SPAN);
			if ((pDestCol->pDflt == 0) != (pSrcCol->pDflt == 0)
			    || (pDestCol->pDflt
				&& strcmp(pDestCol->pDflt->u.zToken,
					  pSrcCol->pDflt->u.zToken) != 0)
			    ) {
				return 0;	/* Default values must be the same for all columns */
			}
		}
	}
	for (pDestIdx = pDest->pIndex; pDestIdx; pDestIdx = pDestIdx->pNext) {
		if (IsUniqueIndex(pDestIdx)) {
			destHasUniqueIdx = 1;
		}
		for (pSrcIdx = pSrc->pIndex; pSrcIdx; pSrcIdx = pSrcIdx->pNext) {
			if (xferCompatibleIndex(pDestIdx, pSrcIdx))
				break;
		}
		if (pSrcIdx == 0) {
			return 0;	/* pDestIdx has no corresponding index in pSrc */
		}
	}
#ifndef SQLITE_OMIT_CHECK
	if (pDest->pCheck
	    && sqlite3ExprListCompare(pSrc->pCheck, pDest->pCheck, -1)) {
		return 0;	/* Tables have different CHECK constraints.  Ticket #2252 */
	}
#endif
#ifndef SQLITE_OMIT_FOREIGN_KEY
	/* Disallow the transfer optimization if the destination table constains
	 * any foreign key constraints.  This is more restrictive than necessary.
	 * So the extra complication to make this rule less restrictive is probably
	 * not worth the effort.  Ticket [6284df89debdfa61db8073e062908af0c9b6118e]
	 */
	if ((user_session->sql_flags & SQLITE_ForeignKeys) != 0
	    && pDest->pFKey != 0) {
		return 0;
	}
#endif
	if ((user_session->sql_flags & SQLITE_CountRows) != 0) {
		return 0;	/* xfer opt does not play well with PRAGMA count_changes */
	}

	/* If we get this far, it means that the xfer optimization is at
	 * least a possibility, though it might only work if the destination
	 * table (tab1) is initially empty.
	 */
#ifdef SQLITE_TEST
	sqlite3_xferopt_count++;
#endif
	v = sqlite3GetVdbe(pParse);
	sqlite3CodeVerifySchema(pParse);
	iSrc = pParse->nTab++;
	iDest = pParse->nTab++;
	regData = sqlite3GetTempReg(pParse);
	regTupleid = sqlite3GetTempReg(pParse);
	sqlite3OpenTable(pParse, iDest, pDest, OP_OpenWrite);
	assert(destHasUniqueIdx);
	if ((pDest->iPKey < 0 && pDest->pIndex != 0)	/* (1) */
	    ||destHasUniqueIdx	/* (2) */
	    || (onError != ON_CONFLICT_ACTION_ABORT
		&& onError != ON_CONFLICT_ACTION_ROLLBACK)	/* (3) */
	    ) {
		/* In some circumstances, we are able to run the xfer optimization
		 * only if the destination table is initially empty.
		 * This block generates code to make
		 * that determination.
		 *
		 * Conditions under which the destination must be empty:
		 *
		 * (1) There is no INTEGER PRIMARY KEY but there are indices.
		 *
		 * (2) The destination has a unique index.  (The xfer optimization
		 *     is unable to test uniqueness.)
		 *
		 * (3) onError is something other than ON_CONFLICT_ACTION_ABORT and _ROLLBACK.
		 */
		addr1 = sqlite3VdbeAddOp2(v, OP_Rewind, iDest, 0);
		VdbeCoverage(v);
		emptyDestTest = sqlite3VdbeAddOp0(v, OP_Goto);
		sqlite3VdbeJumpHere(v, addr1);
	}

	for (pDestIdx = pDest->pIndex; pDestIdx; pDestIdx = pDestIdx->pNext) {
		u8 idxInsFlags = 0;
		for (pSrcIdx = pSrc->pIndex; ALWAYS(pSrcIdx);
		     pSrcIdx = pSrcIdx->pNext) {
			if (xferCompatibleIndex(pDestIdx, pSrcIdx))
				break;
		}
		assert(pSrcIdx);
		struct space *space_src =
			space_by_id(SQLITE_PAGENO_TO_SPACEID(pSrcIdx->tnum));
		assert(space_src != NULL);
		int space_src_ptr_reg = ++pParse->nMem;
		sqlite3VdbeAddOp4Int64(v, OP_Int64, 0, space_src_ptr_reg, 0,
				       ((int64_t) space_src));
		sqlite3VdbeAddOp3(v, OP_OpenRead, iSrc, pSrcIdx->tnum,
				  space_src_ptr_reg);
		sqlite3VdbeSetP4KeyInfo(pParse, pSrcIdx);
		VdbeComment((v, "%s", pSrcIdx->zName));
		struct space *space_dest =
			space_by_id(SQLITE_PAGENO_TO_SPACEID(pDestIdx->tnum));
		assert(space_dest != NULL);
		int space_dest_ptr_reg = ++pParse->nMem;
		sqlite3VdbeAddOp4Int64(v, OP_Int64, 0, space_dest_ptr_reg, 0,
				       ((int64_t) space_dest));
		sqlite3VdbeAddOp3(v, OP_OpenWrite, iDest, pDestIdx->tnum,
				  space_dest_ptr_reg);
		sqlite3VdbeSetP4KeyInfo(pParse, pDestIdx);
		sqlite3VdbeChangeP5(v, OPFLAG_BULKCSR);
		VdbeComment((v, "%s", pDestIdx->zName));
		addr1 = sqlite3VdbeAddOp2(v, OP_Rewind, iSrc, 0);
		VdbeCoverage(v);
		sqlite3VdbeAddOp2(v, OP_RowData, iSrc, regData);
		if (pDestIdx->idxType == 2) {
			idxInsFlags |= OPFLAG_NCHANGE;
		}
		sqlite3VdbeAddOp2(v, OP_IdxInsert, iDest, regData);
		sqlite3VdbeChangeP5(v, idxInsFlags | OPFLAG_APPEND);
		sqlite3VdbeAddOp2(v, OP_Next, iSrc, addr1 + 1);
		VdbeCoverage(v);
		sqlite3VdbeJumpHere(v, addr1);
		sqlite3VdbeAddOp2(v, OP_Close, iSrc, 0);
		sqlite3VdbeAddOp2(v, OP_Close, iDest, 0);
	}
	if (emptySrcTest)
		sqlite3VdbeJumpHere(v, emptySrcTest);
	sqlite3ReleaseTempReg(pParse, regTupleid);
	sqlite3ReleaseTempReg(pParse, regData);
	if (emptyDestTest) {
		sqlite3VdbeAddOp2(v, OP_Halt, SQLITE_OK, 0);
		sqlite3VdbeJumpHere(v, emptyDestTest);
		sqlite3VdbeAddOp2(v, OP_Close, iDest, 0);
		return 0;
	} else {
		return 1;
	}
}
#endif				/* SQLITE_OMIT_XFER_OPT */
