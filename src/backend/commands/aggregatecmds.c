/*-------------------------------------------------------------------------
 *
 * aggregatecmds.c
 *
 *	  Routines for aggregate-manipulation commands
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/aggregatecmds.c
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 *	DefineAggregate
 *
 * "oldstyle" signals the old (pre-8.2) style where the aggregate input type
 * is specified by a BASETYPE element in the parameters.  Otherwise,
 * "args" defines the input type(s).
 */
void
DefineAggregate(List *name, List *args, bool oldstyle, List *parameters)
{
	char	   *aggName;
	Oid			aggNamespace;
	AclResult	aclresult;
	List	   *transfuncName = NIL;
	List	   *finalfuncName = NIL;
	List	   *sortoperatorName = NIL;
	TypeName   *baseType = NULL;
	TypeName   *transType = NULL;
#ifdef XCP
	TypeName   *collectType = NULL;
#endif
	char	   *initval = NULL;
#ifdef PGXC
	List	   *collectfuncName = NIL;
	char	   *initcollect = NULL;
#endif
	Oid		   *aggArgTypes;
	int			numArgs;
	Oid			transTypeId;
#ifdef XCP
	Oid			collectTypeId;
#endif
	ListCell   *pl;

	/* Convert list of names to a name and namespace */
	aggNamespace = QualifiedNameGetCreationNamespace(name, &aggName);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(aggNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(aggNamespace));

	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		/*
		 * sfunc1, stype1, and initcond1 are accepted as obsolete spellings
		 * for sfunc, stype, initcond.
		 */
		if (pg_strcasecmp(defel->defname, "sfunc") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "sfunc1") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "finalfunc") == 0)
			finalfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "sortop") == 0)
			sortoperatorName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "basetype") == 0)
			baseType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "stype") == 0)
			transType = defGetTypeName(defel);
		else if (pg_strcasecmp(defel->defname, "stype1") == 0)
			transType = defGetTypeName(defel);
#ifdef XCP
		else if (pg_strcasecmp(defel->defname, "ctype") == 0)
			collectType = defGetTypeName(defel);
#endif
		else if (pg_strcasecmp(defel->defname, "initcond") == 0)
			initval = defGetString(defel);
		else if (pg_strcasecmp(defel->defname, "initcond1") == 0)
			initval = defGetString(defel);
#ifdef PGXC
		else if (pg_strcasecmp(defel->defname, "cfunc") == 0)
			collectfuncName = defGetQualifiedName(defel);
		else if (pg_strcasecmp(defel->defname, "initcollect") == 0)
			initcollect = defGetString(defel);
#endif
		else
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("aggregate attribute \"%s\" not recognized",
							defel->defname)));
	}

	/*
	 * make sure we have our required definitions
	 */
	if (transType == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate stype must be specified")));
	if (transfuncName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate sfunc must be specified")));

#ifdef XCP
	if (collectfuncName && collectType == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("if aggregate cfunc is defined aggregate ctype must be specified")));
	if (collectType && collectfuncName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("if aggregate ctype is defined aggregate cfunc must be specified")));
#endif

	/*
	 * look up the aggregate's input datatype(s).
	 */
	if (oldstyle)
	{
		/*
		 * Old style: use basetype parameter.  This supports aggregates of
		 * zero or one input, with input type ANY meaning zero inputs.
		 *
		 * Historically we allowed the command to look like basetype = 'ANY'
		 * so we must do a case-insensitive comparison for the name ANY. Ugh.
		 */
		if (baseType == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate input type must be specified")));

		if (pg_strcasecmp(TypeNameToString(baseType), "ANY") == 0)
		{
			numArgs = 0;
			aggArgTypes = NULL;
		}
		else
		{
			numArgs = 1;
			aggArgTypes = (Oid *) palloc(sizeof(Oid));
			aggArgTypes[0] = typenameTypeId(NULL, baseType);
		}
	}
	else
	{
		/*
		 * New style: args is a list of TypeNames (possibly zero of 'em).
		 */
		ListCell   *lc;
		int			i = 0;

		if (baseType != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("basetype is redundant with aggregate input type specification")));

		numArgs = list_length(args);
		aggArgTypes = (Oid *) palloc(sizeof(Oid) * numArgs);
		foreach(lc, args)
		{
			TypeName   *curTypeName = (TypeName *) lfirst(lc);

			aggArgTypes[i++] = typenameTypeId(NULL, curTypeName);
		}
	}

	/*
	 * look up the aggregate's transtype.
	 *
	 * transtype can't be a pseudo-type, since we need to be able to store
	 * values of the transtype.  However, we can allow polymorphic transtype
	 * in some cases (AggregateCreate will check).	Also, we allow "internal"
	 * for functions that want to pass pointers to private data structures;
	 * but allow that only to superusers, since you could crash the system (or
	 * worse) by connecting up incompatible internal-using functions in an
	 * aggregate.
	 */
	transTypeId = typenameTypeId(NULL, transType);
	if (get_typtype(transTypeId) == TYPTYPE_PSEUDO &&
		!IsPolymorphicType(transTypeId))
	{
		if (transTypeId == INTERNALOID && superuser())
			 /* okay */ ;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate transition data type cannot be %s",
							format_type_be(transTypeId))));
	}

#ifdef XCP
	/*
	 * look up the aggregate's collecttype.
	 *
	 * to the collecttype applied all the limitations as to the transtype.
	 */
	if (collectType)
	{
		collectTypeId = typenameTypeId(NULL, collectType);
		if (get_typtype(collectTypeId) == TYPTYPE_PSEUDO &&
			!IsPolymorphicType(collectTypeId))
		{
			if (collectTypeId == INTERNALOID && superuser())
				 /* okay */ ;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("aggregate collection data type cannot be %s",
								format_type_be(collectTypeId))));
		}
	}
	else
		collectTypeId = InvalidOid;
#endif

	/*
	 * Most of the argument-checking is done inside of AggregateCreate
	 */
	AggregateCreate(aggName,	/* aggregate name */
					aggNamespace,		/* namespace */
					aggArgTypes,	/* input data type(s) */
					numArgs,
					transfuncName,		/* step function name */
#ifdef PGXC
					collectfuncName,	/* collect function name */
#endif
					finalfuncName,		/* final function name */
					sortoperatorName,	/* sort operator name */
					transTypeId,	/* transition data type */
#ifdef XCP
					collectTypeId,	/* collection data type */
#endif
#ifdef PGXC
					initval,	/* initial condition */
					initcollect);	/* initial condition for collection function */
#else
					initval);	/* initial condition */
#endif
}


/*
 * RenameAggregate
 *		Rename an aggregate.
 */
void
RenameAggregate(List *name, List *args, const char *newname)
{
	Oid			procOid;
	Oid			namespaceOid;
	HeapTuple	tup;
	Form_pg_proc procForm;
	Relation	rel;
	AclResult	aclresult;

	rel = heap_open(ProcedureRelationId, RowExclusiveLock);

	/* Look up function and make sure it's an aggregate */
	procOid = LookupAggNameTypeNames(name, args, false);

	tup = SearchSysCacheCopy1(PROCOID, ObjectIdGetDatum(procOid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for function %u", procOid);
	procForm = (Form_pg_proc) GETSTRUCT(tup);

	namespaceOid = procForm->pronamespace;

	/* make sure the new name doesn't exist */
	if (SearchSysCacheExists3(PROCNAMEARGSNSP,
							  CStringGetDatum(newname),
							  PointerGetDatum(&procForm->proargtypes),
							  ObjectIdGetDatum(namespaceOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FUNCTION),
				 errmsg("function %s already exists in schema \"%s\"",
						funcname_signature_string(newname,
												  procForm->pronargs,
												  NIL,
											   procForm->proargtypes.values),
						get_namespace_name(namespaceOid))));

	/* must be owner */
	if (!pg_proc_ownercheck(procOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC,
					   NameListToString(name));

	/* must have CREATE privilege on namespace */
	aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceOid));

	/* rename */
	namestrcpy(&(((Form_pg_proc) GETSTRUCT(tup))->proname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);
}

/*
 * Change aggregate owner
 */
void
AlterAggregateOwner(List *name, List *args, Oid newOwnerId)
{
	Oid			procOid;

	/* Look up function and make sure it's an aggregate */
	procOid = LookupAggNameTypeNames(name, args, false);

	/* The rest is just like a function */
	AlterFunctionOwner_oid(procOid, newOwnerId);
}
