/*
 * tclOO.c --
 *
 *	This file contains the method call chain management code for the
 *	object-system core.
 *
 * Copyright (c) 2005-2006 by Donal K. Fellows
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclOOCall.c,v 1.18 2008/05/18 20:33:31 dkf Exp $
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "tclInt.h"
#include "tclOOInt.h"

/*
 * Structure containing a CallContext and any other values needed only during
 * the construction of the CallContext.
 */

struct ChainBuilder {
    CallChain *callChainPtr;	/* The call chain being built. */
    int filterLength;		/* Number of entries in the call chain that
				 * are due to processing filters and not the
				 * main call chain. */
};

/*
 * Extra flags used for call chain management.
 */

#define DEFINITE_PROTECTED 0x100000
#define DEFINITE_PUBLIC    0x200000
#define KNOWN_STATE	   (DEFINITE_PROTECTED | DEFINITE_PUBLIC)
#define SPECIAL		   (CONSTRUCTOR | DESTRUCTOR)

/*
 * Function declarations for things defined in this file.
 */

static void		AddClassFiltersToCallContext(Object *const oPtr,
			    Class *clsPtr, struct ChainBuilder *const cbPtr,
			    Tcl_HashTable *const doneFilters);
static void		AddClassMethodNames(Class *clsPtr, const int flags,
			    Tcl_HashTable *const namesPtr);
static inline void	AddMethodToCallChain(Method *const mPtr,
			    struct ChainBuilder *const cbPtr,
			    Tcl_HashTable *const doneFilters,
			    Class *const filterDecl);
static inline void	AddSimpleChainToCallContext(Object *const oPtr,
			    Tcl_Obj *const methodNameObj,
			    struct ChainBuilder *const cbPtr,
			    Tcl_HashTable *const doneFilters, int flags,
			    Class *const filterDecl);
static void		AddSimpleClassChainToCallContext(Class *classPtr,
			    Tcl_Obj *const methodNameObj,
			    struct ChainBuilder *const cbPtr,
			    Tcl_HashTable *const doneFilters, int flags,
			    Class *const filterDecl);
static int		CmpStr(const void *ptr1, const void *ptr2);
static inline void	DeleteChain(CallChain *callPtr);
static void		DupMethodNameRep(Tcl_Obj *srcPtr, Tcl_Obj *dstPtr);
static void		FreeMethodNameRep(Tcl_Obj *objPtr);

/*
 * Object type used to manage type caches attached to method names.
 */

static Tcl_ObjType methodNameType = {
    "TclOO method name",
    FreeMethodNameRep,
    DupMethodNameRep,
    NULL,
    NULL
};

/*
 * ----------------------------------------------------------------------
 *
 * TclOODeleteContext --
 *
 *	Destroys a method call-chain context, which should not be in use.
 *
 * ----------------------------------------------------------------------
 */

void
TclOODeleteContext(
    CallContext *contextPtr)
{
    DeleteChain(contextPtr->callPtr);
    TclStackFree(contextPtr->interp, contextPtr);
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOOAllocChainCache, TclOODeleteChainCache --
 *
 *	Create and destroy caches of method call-chains.
 *
 * ----------------------------------------------------------------------
 */

Tcl_HashTable *
TclOOAllocChainCache(void)
{
    Tcl_HashTable *tablePtr = (Tcl_HashTable*) ckalloc(sizeof(Tcl_HashTable));

    Tcl_InitObjHashTable(tablePtr);
    return tablePtr;
}

void
TclOODeleteChainCache(
    Tcl_HashTable *tablePtr)
{
    FOREACH_HASH_DECLS;
    CallChain *callPtr;

    FOREACH_HASH_VALUE(callPtr, tablePtr) {
	if (callPtr) {
	    DeleteChain(callPtr);
	}
    }
    Tcl_DeleteHashTable(tablePtr);
    ckfree((char *) tablePtr);
}

/*
 * ----------------------------------------------------------------------
 *
 * DeleteChain --
 *
 *	Destroys a method call-chain.
 *
 * ----------------------------------------------------------------------
 */

static inline void
DeleteChain(
    CallChain *callPtr)
{
    if (--callPtr->refCount >= 1) {
	return;
    }
    if (callPtr->chain != callPtr->staticChain) {
	ckfree((char *) callPtr->chain);
    }
    ckfree((char *) callPtr);
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOOStashContext --
 *
 *	Saves a reference to a method call context in a Tcl_Obj's internal
 *	representation.
 *
 * ----------------------------------------------------------------------
 */

void
TclOOStashContext(
    Tcl_Obj *objPtr,
    CallContext *contextPtr)
{
    contextPtr->callPtr->refCount++;
    if (objPtr->typePtr && objPtr->typePtr->freeIntRepProc) {
	objPtr->typePtr->freeIntRepProc(objPtr);
    }
    objPtr->typePtr = &methodNameType;
    objPtr->internalRep.otherValuePtr = contextPtr->callPtr;
}

/*
 * ----------------------------------------------------------------------
 *
 * DupMethodNameRep, FreeMethodNameRep --
 *
 *	Functions to implement the required parts of the Tcl_Obj guts needed
 *	for caching of method contexts in Tcl_Objs.
 *
 * ----------------------------------------------------------------------
 */

static void
DupMethodNameRep(
    Tcl_Obj *srcPtr,
    Tcl_Obj *dstPtr)
{
    register CallChain *callPtr = srcPtr->internalRep.otherValuePtr;

    dstPtr->typePtr = &methodNameType;
    dstPtr->internalRep.otherValuePtr = callPtr;
    callPtr->refCount++;
}

static void
FreeMethodNameRep(
    Tcl_Obj *objPtr)
{
    register CallChain *callPtr = objPtr->internalRep.otherValuePtr;

    DeleteChain(callPtr);
    objPtr->internalRep.otherValuePtr = NULL;
    objPtr->typePtr = NULL;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOOInvokeContext --
 *
 *	Invokes a single step along a method call-chain context. Note that the
 *	invokation of a step along the chain can cause further steps along the
 *	chain to be invoked. Note that this function is written to be as light
 *	in stack usage as possible.
 *
 * ----------------------------------------------------------------------
 */

int
TclOOInvokeContext(
    Tcl_Interp *const interp,	/* Interpreter for error reporting, and many
				 * other sorts of context handling (e.g.,
				 * commands, variables) depending on method
				 * implementation. */
    CallContext *const contextPtr,
				/* The method call context. */
    const int objc,		/* The number of arguments. */
    Tcl_Obj *const *const objv)	/* The arguments as actually seen. */
{
    Method *const mPtr = contextPtr->callPtr->chain[contextPtr->index].mPtr;
    const int isFirst = (contextPtr->index == 0);
    const int isFilter =
	    contextPtr->callPtr->chain[contextPtr->index].isFilter;
    int result, wasFilter;

    /*
     * If this is the first step along the chain, we preserve the method
     * entries in the chain so that they do not get deleted out from under our
     * feet.
     */

    if (isFirst) {
	int i;

	for (i=0 ; i<contextPtr->callPtr->numChain ; i++) {
	    Tcl_Preserve(contextPtr->callPtr->chain[i].mPtr);
	}

	/*
	 * Ensure that the method name itself is part of the arguments when
	 * we're doing unknown processing.
	 */

	if (contextPtr->callPtr->flags & OO_UNKNOWN_METHOD) {
	    contextPtr->skip--;
	}
    }

    /*
     * Save whether we were in a filter and set up whether we are now.
     */

    wasFilter = contextPtr->callPtr->oPtr->flags & FILTER_HANDLING;
    if (isFilter || contextPtr->callPtr->flags & FILTER_HANDLING) {
	contextPtr->callPtr->oPtr->flags |= FILTER_HANDLING;
    } else {
	contextPtr->callPtr->oPtr->flags &= ~FILTER_HANDLING;
    }

    /*
     * Run the method implementation.
     */

    result = mPtr->typePtr->callProc(mPtr->clientData, interp,
	    (Tcl_ObjectContext) contextPtr, objc, objv);

    /*
     * Restore the old filter-ness, release any locks on method
     * implementations, and return the result code.
     */

    if (wasFilter) {
	contextPtr->callPtr->oPtr->flags |= FILTER_HANDLING;
    } else {
	contextPtr->callPtr->oPtr->flags &= ~FILTER_HANDLING;
    }
    if (isFirst) {
	int i;

	for (i=0 ; i<contextPtr->callPtr->numChain ; i++) {
	    Tcl_Release(contextPtr->callPtr->chain[i].mPtr);
	}
    }
    return result;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOOGetSortedMethodList --
 *
 *	Discovers the list of method names supported by an object.
 *
 * ----------------------------------------------------------------------
 */

int
TclOOGetSortedMethodList(
    Object *oPtr,		/* The object to get the method names for. */
    int flags,			/* Whether we just want the public method
				 * names. */
    const char ***stringsPtr)	/* Where to write a pointer to the array of
				 * strings to. */
{
    Tcl_HashTable names;	/* Tcl_Obj* method name to "wanted in list"
				 * mapping. */
    FOREACH_HASH_DECLS;
    int i;
    Class *mixinPtr;
    Tcl_Obj *namePtr;
    Method *mPtr;
    int isWantedIn;
    void *isWanted;

    Tcl_InitObjHashTable(&names);

    /*
     * Name the bits used in the names table values.
     */
#define IN_LIST 1
#define NO_IMPLEMENTATION 2

    /*
     * Process method names due to the object.
     */

    if (oPtr->methodsPtr) {
	FOREACH_HASH(namePtr, mPtr, oPtr->methodsPtr) {
	    int isNew;

	    if ((mPtr->flags & PRIVATE_METHOD) && !(flags & PRIVATE_METHOD)) {
		continue;
	    }
	    hPtr = Tcl_CreateHashEntry(&names, (char *) namePtr, &isNew);
	    if (isNew) {
		isWantedIn = ((!(flags & PUBLIC_METHOD)
			|| mPtr->flags & PUBLIC_METHOD) ? IN_LIST : 0);
		isWantedIn |= (mPtr->typePtr == NULL ? NO_IMPLEMENTATION : 0);
		Tcl_SetHashValue(hPtr, (void *) isWantedIn);
	    }
	}
    }

    /*
     * Process method names due to private methods on the object's class.
     */

    if (flags & PRIVATE_METHOD) {
	FOREACH_HASH(namePtr, mPtr, &oPtr->selfCls->classMethods) {
	    if (mPtr->flags & PRIVATE_METHOD) {
		int isNew;

		hPtr = Tcl_CreateHashEntry(&names, (char *) namePtr, &isNew);
		if (isNew) {
		    isWantedIn = IN_LIST;
		    if (mPtr->typePtr == NULL) {
			isWantedIn |= NO_IMPLEMENTATION;
		    }
		    Tcl_SetHashValue(hPtr, (void *) isWantedIn);
		} else if (mPtr->typePtr != NULL) {
		    isWantedIn = (int) Tcl_GetHashValue(hPtr);
		    if (isWantedIn & NO_IMPLEMENTATION) {
			isWantedIn &= ~NO_IMPLEMENTATION;
			Tcl_SetHashValue(hPtr, (void *) isWantedIn);
		    }
		}
	    }
	}
    }

    /*
     * Process (normal) method names from the class hierarchy and the mixin
     * hierarchy.
     */

    AddClassMethodNames(oPtr->selfCls, flags, &names);
    FOREACH(mixinPtr, oPtr->mixins) {
	AddClassMethodNames(mixinPtr, flags, &names);
    }

    /*
     * See how many (visible) method names there are. If none, we do not (and
     * should not) try to sort the list of them.
     */

    i = 0;
    if (names.numEntries != 0) {
	const char **strings;

	/*
	 * We need to build the list of methods to sort. We will be using
	 * qsort() for this, because it is very unlikely that the list will be
	 * heavily sorted when it is long enough to matter.
	 */

	strings = (const char **) ckalloc(sizeof(char *) * names.numEntries);
	FOREACH_HASH(namePtr, isWanted, &names) {
	    if (!(flags & PUBLIC_METHOD) || (((int)isWanted) & IN_LIST)) {
		if (((int)isWanted) & NO_IMPLEMENTATION) {
		    continue;
		}
		strings[i++] = TclGetString(namePtr);
	    }
	}

	/*
	 * Note that 'i' may well be less than names.numEntries when we are
	 * dealing with public method names.
	 */

	qsort((void *) strings, (unsigned) i, sizeof(char *), CmpStr);
	*stringsPtr = strings;
    }

    Tcl_DeleteHashTable(&names);
    return i;
}

/* Comparator for GetSortedMethodList */
static int
CmpStr(
    const void *ptr1,
    const void *ptr2)
{
    const char **strPtr1 = (const char **) ptr1;
    const char **strPtr2 = (const char **) ptr2;

    return TclpUtfNcmp2(*strPtr1, *strPtr2, strlen(*strPtr1)+1);
}

/*
 * ----------------------------------------------------------------------
 *
 * AddClassMethodNames --
 *
 *	Adds the method names defined by a class (or its superclasses) to the
 *	collection being built. The collection is built in a hash table to
 *	ensure that duplicates are excluded. Helper for GetSortedMethodList().
 *
 * ----------------------------------------------------------------------
 */

static void
AddClassMethodNames(
    Class *clsPtr,		/* Class to get method names from. */
    const int flags,		/* Whether we are interested in just the
				 * public method names. */
    Tcl_HashTable *const namesPtr)
				/* Reference to the hash table to put the
				 * information in. The hash table maps the
				 * Tcl_Obj * method name to an integral value
				 * describing whether the method is wanted.
				 * This ensures that public/private override
				 * semantics are handled correctly.*/
{
    /*
     * Scope all declarations so that the compiler can stand a good chance of
     * making the recursive step highly efficient. We also hand-implement the
     * tail-recursive case using a while loop; C compilers typically cannot do
     * tail-recursion optimization usefully.
     */

    if (clsPtr->mixins.num != 0) {
	Class *mixinPtr;
	int i;

	/* TODO: Beware of infinite loops! */
	FOREACH(mixinPtr, clsPtr->mixins) {
	    AddClassMethodNames(mixinPtr, flags, namesPtr);
	}
    }

    while (1) {
	FOREACH_HASH_DECLS;
	Tcl_Obj *namePtr;
	Method *mPtr;

	FOREACH_HASH(namePtr, mPtr, &clsPtr->classMethods) {
	    int isNew;

	    hPtr = Tcl_CreateHashEntry(namesPtr, (char *) namePtr, &isNew);
	    if (isNew) {
		int isWanted = (!(flags & PUBLIC_METHOD)
			|| (mPtr->flags & PUBLIC_METHOD)) ? IN_LIST : 0;

		Tcl_SetHashValue(hPtr, (void *) isWanted);
	    } else if ((((int)Tcl_GetHashValue(hPtr)) & NO_IMPLEMENTATION)
		    && mPtr->typePtr != NULL) {
		int isWanted = (int) Tcl_GetHashValue(hPtr);

		isWanted &= ~NO_IMPLEMENTATION;
		Tcl_SetHashValue(hPtr, (void *) isWanted);
	    }
	}

	if (clsPtr->superclasses.num != 1) {
	    break;
	}
	clsPtr = clsPtr->superclasses.list[0];
    }
    if (clsPtr->superclasses.num != 0) {
	Class *superPtr;
	int i;

	FOREACH(superPtr, clsPtr->superclasses) {
	    AddClassMethodNames(superPtr, flags, namesPtr);
	}
    }
}

/*
 * ----------------------------------------------------------------------
 *
 * AddSimpleChainToCallContext --
 *
 *	The core of the call-chain construction engine, this handles calling a
 *	particular method on a particular object. Note that filters and
 *	unknown handling are already handled by the logic that uses this
 *	function.
 *
 * ----------------------------------------------------------------------
 */

static inline void
AddSimpleChainToCallContext(
    Object *const oPtr,		/* Object to add call chain entries for. */
    Tcl_Obj *const methodNameObj,
				/* Name of method to add the call chain
				 * entries for. */
    struct ChainBuilder *const cbPtr,
				/* Where to add the call chain entries. */
    Tcl_HashTable *const doneFilters,
				/* Where to record what call chain entries
				 * have been processed. */
    int flags,			/* What sort of call chain are we building. */
    Class *const filterDecl)	/* The class that declared the filter. If
				 * NULL, either the filter was declared by the
				 * object or this isn't a filter. */
{
    int i;

    if (!(flags & (KNOWN_STATE | SPECIAL)) && oPtr->methodsPtr) {
	Tcl_HashEntry *hPtr = Tcl_FindHashEntry(oPtr->methodsPtr,
		(char *) methodNameObj);

	if (hPtr != NULL) {
	    Method *mPtr = Tcl_GetHashValue(hPtr);

	    if (flags & PUBLIC_METHOD) {
		if (!(mPtr->flags & PUBLIC_METHOD)) {
		    return;
		} else {
		    flags |= DEFINITE_PUBLIC;
		}
	    } else {
		flags |= DEFINITE_PROTECTED;
	    }
	}
    }
    if (!(flags & SPECIAL)) {
	Tcl_HashEntry *hPtr;
	Class *mixinPtr;

	FOREACH(mixinPtr, oPtr->mixins) {
	    AddSimpleClassChainToCallContext(mixinPtr, methodNameObj, cbPtr,
		    doneFilters, flags, filterDecl);
	}
	if (oPtr->methodsPtr) {
	    hPtr = Tcl_FindHashEntry(oPtr->methodsPtr, (char*) methodNameObj);
	    if (hPtr != NULL) {
		AddMethodToCallChain(Tcl_GetHashValue(hPtr), cbPtr,
			doneFilters, filterDecl);
	    }
	}
    }
    AddSimpleClassChainToCallContext(oPtr->selfCls, methodNameObj, cbPtr,
	    doneFilters, flags, filterDecl);
}

/*
 * ----------------------------------------------------------------------
 *
 * AddMethodToCallChain --
 *
 *	Utility method that manages the adding of a particular method
 *	implementation to a call-chain.
 *
 * ----------------------------------------------------------------------
 */

static inline void
AddMethodToCallChain(
    Method *const mPtr,		/* Actual method implementation to add to call
				 * chain (or NULL, a no-op). */
    struct ChainBuilder *const cbPtr,
				/* The call chain to add the method
				 * implementation to. */
    Tcl_HashTable *const doneFilters,
				/* Where to record what filters have been
				 * processed. If NULL, not processing filters.
				 * Note that this function does not update
				 * this hashtable. */
    Class *const filterDecl)	/* The class that declared the filter. If
				 * NULL, either the filter was declared by the
				 * object or this isn't a filter. */
{
    register CallChain *callPtr = cbPtr->callChainPtr;
    int i;

    /*
     * Return if this is just an entry used to record whether this is a public
     * method. If so, there's nothing real to call and so nothing to add to
     * the call chain.
     */

    if (mPtr == NULL || mPtr->typePtr == NULL) {
	return;
    }

    /*
     * Enforce real private method handling here. We will skip adding this
     * method IF
     *  1) we are not allowing private methods, AND
     *  2) this is a private method, AND
     *  3) this is a class method, AND
     *  4) this method was not declared by the class of the current object.
     *
     * This does mean that only classes really handle private methods. This
     * should be sufficient for [incr Tcl] support though.
     */

    if (!(callPtr->flags & PRIVATE_METHOD)
	    && (mPtr->flags & PRIVATE_METHOD)
	    && (mPtr->declaringClassPtr != NULL)
	    && (mPtr->declaringClassPtr != callPtr->oPtr->selfCls)) {
	return;
    }

    /*
     * First test whether the method is already in the call chain. Skip over
     * any leading filters.
     */

    for (i=cbPtr->filterLength ; i<callPtr->numChain ; i++) {
	if (callPtr->chain[i].mPtr == mPtr &&
		callPtr->chain[i].isFilter == (doneFilters != NULL)) {
	    /*
	     * Call chain semantics states that methods come as *late* in the
	     * call chain as possible. This is done by copying down the
	     * following methods. Note that this does not change the number of
	     * method invokations in the call chain; it just rearranges them.
	     */

	    Class *declCls = callPtr->chain[i].filterDeclarer;

	    for (; i+1<callPtr->numChain ; i++) {
		callPtr->chain[i] = callPtr->chain[i+1];
	    }
	    callPtr->chain[i].mPtr = mPtr;
	    callPtr->chain[i].isFilter = (doneFilters != NULL);
	    callPtr->chain[i].filterDeclarer = declCls;
	    return;
	}
    }

    /*
     * Need to really add the method. This is made a bit more complex by the
     * fact that we are using some "static" space initially, and only start
     * realloc-ing if the chain gets long.
     */

    if (callPtr->numChain == CALL_CHAIN_STATIC_SIZE) {
	callPtr->chain = (struct MInvoke *)
		ckalloc(sizeof(struct MInvoke)*(callPtr->numChain+1));
	memcpy(callPtr->chain, callPtr->staticChain,
		sizeof(struct MInvoke) * callPtr->numChain);
    } else if (callPtr->numChain > CALL_CHAIN_STATIC_SIZE) {
	callPtr->chain = (struct MInvoke *) ckrealloc((char *) callPtr->chain,
		sizeof(struct MInvoke) * (callPtr->numChain + 1));
    }
    callPtr->chain[i].mPtr = mPtr;
    callPtr->chain[i].isFilter = (doneFilters != NULL);
    callPtr->chain[i].filterDeclarer = filterDecl;
    callPtr->numChain++;
}

/*
 * ----------------------------------------------------------------------
 *
 * InitCallChain --
 *	Encoding of the policy of how to set up a call chain. Doesn't populate
 *	the chain with the method implementation data.
 *
 * ----------------------------------------------------------------------
 */

static inline void
InitCallChain(
    CallChain *callPtr,
    Foundation *fPtr,
    Object *oPtr,
    int flags)
{
    callPtr->epoch = fPtr->epoch;
    callPtr->oPtr = oPtr;
    callPtr->objectCreationEpoch = oPtr->creationEpoch;
    callPtr->objectEpoch = oPtr->epoch;
    callPtr->flags = 0;
    if (flags & (PUBLIC_METHOD|PRIVATE_METHOD|SPECIAL|FILTER_HANDLING)) {
	callPtr->flags |=
		flags&(PUBLIC_METHOD|PRIVATE_METHOD|SPECIAL|FILTER_HANDLING);
    }
    callPtr->refCount = 1;
    callPtr->numChain = 0;
    callPtr->chain = callPtr->staticChain;
}

/*
 * ----------------------------------------------------------------------
 *
 * TclOOGetCallContext --
 *
 *	Responsible for constructing the call context, an ordered list of all
 *	method implementations to be called as part of a method invokation.
 *	This method is central to the whole operation of the OO system.
 *
 * ----------------------------------------------------------------------
 */

CallContext *
TclOOGetCallContext(
    Foundation *fPtr,		/* The foundation of the object system. */
    Object *oPtr,		/* The object to get the context for. */
    Tcl_Obj *methodNameObj,	/* The name of the method to get the context
				 * for. NULL when getting a constructor or
				 * destructor chain. */
    int flags,			/* What sort of context are we looking for.
				 * Only the bits PUBLIC_METHOD, CONSTRUCTOR,
				 * PRIVATE_METHOD, DESTRUCTOR and
				 * FILTER_HANDLING are useful. */
    Tcl_HashTable *cachePtr)	/* Where to cache the chain. Ignored for both
				 * constructors and destructors. */
{
    CallContext *contextPtr;
    CallChain *callPtr;
    struct ChainBuilder cb;
    int i, count, doFilters;
    Tcl_HashEntry *hPtr;
    Tcl_HashTable doneFilters;

    if (flags&(SPECIAL|FILTER_HANDLING) || (oPtr->flags&FILTER_HANDLING)) {
	hPtr = NULL;
	doFilters = 0;
    } else {
	/*
	 * Check if we can get the chain out of the Tcl_Obj method name or out
	 * of the cache.
	 *
	 * The condition on a chain from a cached location being reusable is:
	 *	Refers to the same object (same creation epoch), and
	 *	Still across the same class structure (same global epoch), and
	 *	Still across the same object strucutre (same local epoch), and
	 *	No public/private/filter magic leakage (same flags, modulo the
	 *	fact that a public chain will satisfy a non-public call).
	 */

	const int reuseMask = ((flags & PUBLIC_METHOD) ? ~0 : ~PUBLIC_METHOD);

	if (methodNameObj->typePtr == &methodNameType) {
	    callPtr = methodNameObj->internalRep.otherValuePtr;
	    if ((callPtr->objectCreationEpoch == oPtr->creationEpoch)
		    && (callPtr->epoch == fPtr->epoch)
		    && (callPtr->objectEpoch == oPtr->epoch)
		    && ((callPtr->flags&reuseMask) == (flags&reuseMask))) {
		callPtr->refCount++;
		goto returnContext;
	    }
	    methodNameObj->typePtr->freeIntRepProc(methodNameObj);
	}
	hPtr = Tcl_FindHashEntry(cachePtr, (char *) methodNameObj);
	if (hPtr != NULL && Tcl_GetHashValue(hPtr) != NULL) {
	    callPtr = Tcl_GetHashValue(hPtr);
	    if ((callPtr->objectCreationEpoch == oPtr->creationEpoch)
		    && (callPtr->epoch == fPtr->epoch)
		    && (callPtr->objectEpoch == oPtr->epoch)
		    && ((callPtr->flags&reuseMask) == (flags&reuseMask))) {
		callPtr->refCount++;
		goto returnContext;
	    }
	    Tcl_SetHashValue(hPtr, NULL);
	    DeleteChain(callPtr);
	}
	doFilters = 1;
    }

    callPtr = (CallChain *) ckalloc(sizeof(CallChain));
    InitCallChain(callPtr, fPtr, oPtr, flags);

    cb.callChainPtr = callPtr;
    cb.filterLength = 0;

    /*
     * Add all defined filters (if any, and if we're going to be processing
     * them; they're not processed for constructors, destructors or when we're
     * in the middle of processing a filter).
     */

    if (doFilters) {
	Tcl_Obj *filterObj;
	Class *mixinPtr;

	doFilters = 1;
	Tcl_InitObjHashTable(&doneFilters);
	FOREACH(mixinPtr, oPtr->mixins) {
	    AddClassFiltersToCallContext(oPtr, mixinPtr, &cb, &doneFilters);
	}
	FOREACH(filterObj, oPtr->filters) {
	    AddSimpleChainToCallContext(oPtr, filterObj, &cb, &doneFilters, 0,
		    NULL);
	}
	AddClassFiltersToCallContext(oPtr, oPtr->selfCls, &cb, &doneFilters);
	Tcl_DeleteHashTable(&doneFilters);
    }
    count = cb.filterLength = callPtr->numChain;

    /*
     * Add the actual method implementations.
     */

    AddSimpleChainToCallContext(oPtr, methodNameObj, &cb, NULL, flags, NULL);

    /*
     * Check to see if the method has no implementation. If so, we probably
     * need to add in a call to the unknown method. Otherwise, set up the
     * cacheing of the method implementation (if relevant).
     */

    if (count == callPtr->numChain) {
	/*
	 * Method does not actually exist. If we're dealing with constructors
	 * or destructors, this isn't a problem.
	 */

	if (flags & SPECIAL) {
	    DeleteChain(callPtr);
	    return NULL;
	}
	AddSimpleChainToCallContext(oPtr, fPtr->unknownMethodNameObj, &cb,
		NULL, 0, NULL);
	callPtr->flags |= OO_UNKNOWN_METHOD;
	callPtr->epoch = -1;
	if (count == callPtr->numChain) {
	    DeleteChain(callPtr);
	    return NULL;
	}
    } else if (doFilters) {
	if (hPtr == NULL) {
	    hPtr = Tcl_CreateHashEntry(cachePtr, (char *) methodNameObj, &i);
	}
	callPtr->refCount++;
	Tcl_SetHashValue(hPtr, callPtr);
    }

  returnContext:
    contextPtr = TclStackAlloc(fPtr->interp, sizeof(CallContext));
    contextPtr->interp = fPtr->interp;
    contextPtr->callPtr = callPtr;
    contextPtr->skip = 2;
    contextPtr->index = 0;
    return contextPtr;
}

/*
 * ----------------------------------------------------------------------
 *
 * AddClassFiltersToCallContext --
 *
 *	Logic to make extracting all the filters from the class context much
 *	easier.
 *
 * ----------------------------------------------------------------------
 */

static void
AddClassFiltersToCallContext(
    Object *const oPtr,		/* Object that the filters operate on. */
    Class *clsPtr,		/* Class to get the filters from. */
    struct ChainBuilder *const cbPtr,
				/* Context to fill with call chain entries. */
    Tcl_HashTable *const doneFilters)
				/* Where to record what filters have been
				 * processed. Keys are objects, values are
				 * ignored. */
{
    int i;
    Class *superPtr, *mixinPtr;
    Tcl_Obj *filterObj;

  tailRecurse:
    if (clsPtr == NULL) {
	return;
    }

    /*
     * Add all the filters defined by classes mixed into the main class
     * hierarchy.
     */

    FOREACH(mixinPtr, clsPtr->mixins) {
	AddClassFiltersToCallContext(oPtr, mixinPtr, cbPtr, doneFilters);
    }

    /*
     * Add all the class filters from the current class. Note that the filters
     * are added starting at the object root, as this allows the object to
     * override how filters work to extend their behaviour.
     */

    FOREACH(filterObj, clsPtr->filters) {
	int isNew;

	(void) Tcl_CreateHashEntry(doneFilters, (char *) filterObj, &isNew);
	if (isNew) {
	    AddSimpleChainToCallContext(oPtr, filterObj, cbPtr, doneFilters,
		    0, clsPtr);
	}
    }

    /*
     * Now process the recursive case. Notice the tail-call optimization.
     */

    switch (clsPtr->superclasses.num) {
    case 1:
	clsPtr = clsPtr->superclasses.list[0];
	goto tailRecurse;
    default:
	FOREACH(superPtr, clsPtr->superclasses) {
	    AddClassFiltersToCallContext(oPtr, superPtr, cbPtr, doneFilters);
	}
    case 0:
	return;
    }
}

/*
 * ----------------------------------------------------------------------
 *
 * AddSimpleClassChainToCallContext --
 *
 *	Construct a call-chain from a class hierarchy.
 *
 * ----------------------------------------------------------------------
 */

static void
AddSimpleClassChainToCallContext(
    Class *classPtr,		/* Class to add the call chain entries for. */
    Tcl_Obj *const methodNameObj,
				/* Name of method to add the call chain
				 * entries for. */
    struct ChainBuilder *const cbPtr,
				/* Where to add the call chain entries. */
    Tcl_HashTable *const doneFilters,
				/* Where to record what call chain entries
				 * have been processed. */
    int flags,			/* What sort of call chain are we building. */
    Class *const filterDecl)	/* The class that declared the filter. If
				 * NULL, either the filter was declared by the
				 * object or this isn't a filter. */
{
    int i;
    Class *superPtr;

    /*
     * We hard-code the tail-recursive form. It's by far the most common case
     * *and* it is much more gentle on the stack.
     */

  tailRecurse:
    if (flags & CONSTRUCTOR) {
	AddMethodToCallChain(classPtr->constructorPtr, cbPtr, doneFilters,
		filterDecl);

    } else if (flags & DESTRUCTOR) {
	AddMethodToCallChain(classPtr->destructorPtr, cbPtr, doneFilters,
		filterDecl);
    } else {
	Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&classPtr->classMethods,
		(char *) methodNameObj);

	if (hPtr != NULL) {
	    register Method *mPtr = Tcl_GetHashValue(hPtr);

	    if (!(flags & KNOWN_STATE)) {
		if (flags & PUBLIC_METHOD) {
		    if (mPtr->flags & PUBLIC_METHOD) {
			flags |= DEFINITE_PUBLIC;
		    } else {
			return;
		    }
		} else {
		    flags |= DEFINITE_PROTECTED;
		}
	    }
	    AddMethodToCallChain(mPtr, cbPtr, doneFilters, filterDecl);
	}
    }

    FOREACH(superPtr, classPtr->mixins) {
	AddSimpleClassChainToCallContext(superPtr, methodNameObj, cbPtr,
		doneFilters, flags, filterDecl);
    }

    switch (classPtr->superclasses.num) {
    case 1:
	classPtr = classPtr->superclasses.list[0];
	goto tailRecurse;
    default:
	FOREACH(superPtr, classPtr->superclasses) {
	    AddSimpleClassChainToCallContext(superPtr, methodNameObj, cbPtr,
		    doneFilters, flags, filterDecl);
	}
    case 0:
	return;
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
