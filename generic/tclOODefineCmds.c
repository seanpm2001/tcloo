/*
 * tclOODefineCmds.c --
 *
 *	This file contains the implementation of the ::oo::define command,
 *	part of the object-system core (NB: not Tcl_Obj, but ::oo).
 *
 * Copyright (c) 2006 by Donal K. Fellows
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclOODefineCmds.c,v 1.9 2008/05/08 23:04:30 dkf Exp $
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "tclInt.h"
#include "tclOOInt.h"

static inline void
BumpGlobalEpoch(
    Tcl_Interp *interp,
    Class *classPtr)
{
    if (classPtr != NULL
	    && classPtr->subclasses.num == 0
	    && classPtr->instances.num == 0
	    && classPtr->mixinSubs.num == 0) {
	/*
	 * If a class has no subclasses or instances, and is not mixed into
	 * anything, a change to its structure does not require us to
	 * invalidate any call chains. Note that we still bump our object's
	 * epoch if it has any mixins; the relation between a class and its
	 * representative object is special. But it won't hurt.
	 */

	if (classPtr->thisPtr->mixins.num > 0) {
	    classPtr->thisPtr->epoch++;
	}
	return;
    }

    /*
     * Either there's no class (?!) or we're reconfiguring something that is
     * in use. Force regeneration of call chains.
     */

    TclOOGetFoundation(interp)->epoch++;
}

int
TclOODefineObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const *objv)
{
    CallFrame *framePtr, **framePtrPtr;
    Foundation *fPtr = TclOOGetFoundation(interp);
    int result;
    Object *oPtr;

    if (objc < 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "className arg ?arg ...?");
	return TCL_ERROR;
    }

    oPtr = (Object *) Tcl_GetObjectFromObj(interp, objv[1]);
    if (oPtr == NULL) {
	return TCL_ERROR;
    }
    if (oPtr->classPtr == NULL) {
	Tcl_AppendResult(interp, TclGetString(objv[1]),
		" does not refer to a class", NULL);
	return TCL_ERROR;
    }

    /*
     * Make the oo::define namespace the current namespace and evaluate the
     * command(s).
     */

    /* This is needed to satisfy GCC 3.3's strict aliasing rules */
    framePtrPtr = &framePtr;
    result = TclPushStackFrame(interp, (Tcl_CallFrame **) framePtrPtr,
	    (Tcl_Namespace *) fPtr->defineNs, FRAME_IS_OO_DEFINE);
    if (result != TCL_OK) {
	return TCL_ERROR;
    }
    framePtr->clientData = oPtr;
    framePtr->objc = objc;
    framePtr->objv = objv;	/* Reference counts do not need to be
				 * incremented here. */

    Tcl_Preserve(oPtr);
    if (objc == 3) {
	result = TclEvalObjEx(interp, objv[2], 0,
		((Interp *)interp)->cmdFramePtr, 2);

	if (result == TCL_ERROR) {
	    int length;
	    const char *objName = Tcl_GetStringFromObj(objv[1], &length);
	    int limit = 60;
	    int overflow = (length > limit);

	    Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf(
		    "\n    (in definition script for object \"%.*s%s\" line %d)",
		    (overflow ? limit : length), objName,
		    (overflow ? "..." : ""), interp->errorLine));
	}
    } else {
	Tcl_Obj *objPtr, *obj2Ptr, **objs;
	Interp *iPtr = (Interp *) interp;
	Tcl_Command cmd;
	int dummy;

	/*
	 * More than one argument: fire them through the ensemble processing
	 * engine so that everything appears to be good and proper in error
	 * messages. Note that we cannot just concatenate and send through
	 * Tcl_EvalObjEx, as that doesn't do ensemble processing, and we
	 * cannot go through Tcl_EvalObjv without the extra work to pre-find
	 * the command, as that finds command names in the wrong namespace at
	 * the moment. Ugly!
	 */

	if (iPtr->ensembleRewrite.sourceObjs == NULL) {
	    iPtr->ensembleRewrite.sourceObjs = objv;
	    iPtr->ensembleRewrite.numRemovedObjs = 3;
	    iPtr->ensembleRewrite.numInsertedObjs = 1;
	} else {
	    int ni = iPtr->ensembleRewrite.numInsertedObjs;
	    if (ni < 3) {
		iPtr->ensembleRewrite.numRemovedObjs += 3 - ni;
	    } else {
		iPtr->ensembleRewrite.numInsertedObjs -= 2;
	    }
	}

	/*
	 * Build the list of arguments using a Tcl_Obj as a workspace. See
	 * comments above for why these contortions are necessary.
	 */

	objPtr = Tcl_NewObj();
	obj2Ptr = Tcl_NewObj();
	cmd = Tcl_FindCommand(interp, TclGetString(objv[2]), fPtr->defineNs,
		TCL_NAMESPACE_ONLY);
	if (cmd == NULL) {
	    /* punt this case! */
	    Tcl_AppendObjToObj(obj2Ptr, objv[2]);
	} else {
	    Tcl_GetCommandFullName(interp, cmd, obj2Ptr);
	}
	Tcl_ListObjAppendElement(NULL, objPtr, obj2Ptr);
	Tcl_ListObjReplace(NULL, objPtr, 1, 0, objc-3, objv+3);
	Tcl_ListObjGetElements(NULL, objPtr, &dummy, &objs);

	result = Tcl_EvalObjv(interp, objc-2, objs, TCL_EVAL_INVOKE);
	Tcl_DecrRefCount(objPtr);
    }
    Tcl_Release(oPtr);

    /*
     * Restore the previous "current" namespace.
     */

    TclPopStackFrame(interp);
    return result;
}

int
TclOOObjDefObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const *objv)
{
    CallFrame *framePtr, **framePtrPtr;
    Foundation *fPtr = TclOOGetFoundation(interp);
    int result;
    Object *oPtr;

    if (objc < 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "objectName arg ?arg ...?");
	return TCL_ERROR;
    }

    oPtr = (Object *) Tcl_GetObjectFromObj(interp, objv[1]);
    if (oPtr == NULL) {
	return TCL_ERROR;
    }

    /*
     * Make the oo::objdefine namespace the current namespace and evaluate the
     * command(s).
     */

    /* This is needed to satisfy GCC 3.3's strict aliasing rules */
    framePtrPtr = &framePtr;
    result = TclPushStackFrame(interp, (Tcl_CallFrame **) framePtrPtr,
	    (Tcl_Namespace *) fPtr->objdefNs, FRAME_IS_OO_DEFINE);
    if (result != TCL_OK) {
	return TCL_ERROR;
    }
    framePtr->clientData = oPtr;
    framePtr->objc = objc;
    framePtr->objv = objv;	/* Reference counts do not need to be
				 * incremented here. */

    Tcl_Preserve(oPtr);
    if (objc == 3) {
	result = TclEvalObjEx(interp, objv[2], 0,
		((Interp *)interp)->cmdFramePtr, 2);

	if (result == TCL_ERROR) {
	    int length;
	    const char *objName = Tcl_GetStringFromObj(objv[1], &length);
	    int limit = 60;
	    int overflow = (length > limit);

	    Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf(
		    "\n    (in definition script for object \"%.*s%s\" line %d)",
		    (overflow ? limit : length), objName,
		    (overflow ? "..." : ""), interp->errorLine));
	}
    } else {
	Tcl_Obj *objPtr, *obj2Ptr, **objs;
	Interp *iPtr = (Interp *) interp;
	Tcl_Command cmd;
	int dummy;

	/*
	 * More than one argument: fire them through the ensemble processing
	 * engine so that everything appears to be good and proper in error
	 * messages. Note that we cannot just concatenate and send through
	 * Tcl_EvalObjEx, as that doesn't do ensemble processing, and we
	 * cannot go through Tcl_EvalObjv without the extra work to pre-find
	 * the command, as that finds command names in the wrong namespace at
	 * the moment. Ugly!
	 */

	if (iPtr->ensembleRewrite.sourceObjs == NULL) {
	    iPtr->ensembleRewrite.sourceObjs = objv;
	    iPtr->ensembleRewrite.numRemovedObjs = 3;
	    iPtr->ensembleRewrite.numInsertedObjs = 1;
	} else {
	    int ni = iPtr->ensembleRewrite.numInsertedObjs;
	    if (ni < 3) {
		iPtr->ensembleRewrite.numRemovedObjs += 3 - ni;
	    } else {
		iPtr->ensembleRewrite.numInsertedObjs -= 2;
	    }
	}

	/*
	 * Build the list of arguments using a Tcl_Obj as a workspace. See
	 * comments above for why these contortions are necessary.
	 */

	objPtr = Tcl_NewObj();
	obj2Ptr = Tcl_NewObj();
	cmd = Tcl_FindCommand(interp, TclGetString(objv[2]), fPtr->objdefNs,
		TCL_NAMESPACE_ONLY);
	if (cmd == NULL) {
	    /* punt this case! */
	    Tcl_AppendObjToObj(obj2Ptr, objv[2]);
	} else {
	    Tcl_GetCommandFullName(interp, cmd, obj2Ptr);
	}
	Tcl_ListObjAppendElement(NULL, objPtr, obj2Ptr);
	Tcl_ListObjReplace(NULL, objPtr, 1, 0, objc-3, objv+3);
	Tcl_ListObjGetElements(NULL, objPtr, &dummy, &objs);

	result = Tcl_EvalObjv(interp, objc-2, objs, TCL_EVAL_INVOKE);
	Tcl_DecrRefCount(objPtr);
    }
    Tcl_Release(oPtr);

    /*
     * Restore the previous "current" namespace.
     */

    TclPopStackFrame(interp);
    return result;
}

Tcl_Object
TclOOGetDefineCmdContext(
    Tcl_Interp *interp)
{
    Interp *iPtr = (Interp *) interp;

    if ((iPtr->framePtr == NULL)
	    || (iPtr->framePtr->isProcCallFrame != FRAME_IS_OO_DEFINE)) {
	Tcl_AppendResult(interp, "this command may only be called from within"
		" the context of the ::oo::define command", NULL);
	return NULL;
    }
    return (Tcl_Object) iPtr->framePtr->clientData;
}

int
TclOODefineConstructorObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const *objv)
{
    Object *oPtr;
    Class *clsPtr;
    Tcl_Method method;
    int bodyLength;

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "arguments body");
	return TCL_ERROR;
    }

    /*
     * Extract and validate the context, which is the class that we wish to
     * modify.
     */

    oPtr = (Object *) TclOOGetDefineCmdContext(interp);
    if (oPtr == NULL) {
	return TCL_ERROR;
    }
    clsPtr = oPtr->classPtr;

    (void) Tcl_GetStringFromObj(objv[2], &bodyLength);
    if (bodyLength > 0) {
	/*
	 * Create the method structure.
	 */

	method = (Tcl_Method) TclOONewProcMethod(interp, clsPtr,
		PUBLIC_METHOD, NULL, objv[1], objv[2], NULL);
	if (method == NULL) {
	    return TCL_ERROR;
	}
    } else {
	/*
	 * Delete the constructor method record and set the field in the
	 * class record to NULL.
	 */

	method = NULL;
    }

    /*
     * Place the method structure in the class record. Note that we might not
     * immediately delete the constructor as this might be being done during
     * execution of the constructor itself.
     */

    Tcl_ClassSetConstructor((Tcl_Class) clsPtr, method);
    return TCL_OK;
}

int
TclOODefineDestructorObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const *objv)
{
    Object *oPtr;
    Class *clsPtr;
    Tcl_Method method;
    int bodyLength;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "body");
	return TCL_ERROR;
    }

    oPtr = (Object *) TclOOGetDefineCmdContext(interp);
    if (oPtr == NULL) {
	return TCL_ERROR;
    }
    clsPtr = oPtr->classPtr;

    (void) Tcl_GetStringFromObj(objv[1], &bodyLength);
    if (bodyLength > 0) {
	/*
	 * Create the method structure.
	 */

	method = (Tcl_Method) TclOONewProcMethod(interp, clsPtr,
		PUBLIC_METHOD, NULL, NULL, objv[1], NULL);
	if (method == NULL) {
	    return TCL_ERROR;
	}
    } else {
	/*
	 * Delete the destructor method record and set the field in the class
	 * record to NULL.
	 */

	method = NULL;
    }

    /*
     * Place the method structure in the class record. Note that we might not
     * immediately delete the destructor as this might be being done during
     * execution of the destructor itself. Also note that setting a
     * destructor during a destructor is fairly dumb anyway.
     */

    Tcl_ClassSetDestructor((Tcl_Class) clsPtr, method);
    return TCL_OK;
}

int
TclOODefineExportObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const *objv)
{
    int isSelfExport = (clientData != NULL);
    Object *oPtr;
    Method *mPtr;
    Tcl_HashEntry *hPtr;
    Class *clsPtr;
    int i, isNew;

    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name ?name ...?");
	return TCL_ERROR;
    }

    oPtr = (Object *) TclOOGetDefineCmdContext(interp);
    if (oPtr == NULL) {
	return TCL_ERROR;
    }
    clsPtr = oPtr->classPtr;
    if (!isSelfExport && !clsPtr) {
	Tcl_AppendResult(interp, "attempt to misuse API", NULL);
	return TCL_ERROR;
    }

    for (i=1 ; i<objc ; i++) {
	if (isSelfExport) {
	    hPtr = Tcl_CreateHashEntry(&oPtr->methods, (char *) objv[i],
		    &isNew);
	} else {
	    hPtr = Tcl_CreateHashEntry(&clsPtr->classMethods, (char*) objv[i],
		    &isNew);
	}

	if (isNew) {
	    mPtr = (Method *) ckalloc(sizeof(Method));
	    memset(mPtr, 0, sizeof(Method));
	    Tcl_SetHashValue(hPtr, mPtr);
	} else {
	    mPtr = Tcl_GetHashValue(hPtr);
	}
	mPtr->flags |= PUBLIC_METHOD;
    }
    if (isSelfExport) {
	oPtr->epoch++;
    } else {
	BumpGlobalEpoch(interp, clsPtr);
    }
    return TCL_OK;
}

void
TclOOObjectSetFilters(
    Object *oPtr,
    int numFilters,
    Tcl_Obj *const *filters)
{
    int i;

    if (oPtr->filters.num) {
	Tcl_Obj *filterObj;

	FOREACH(filterObj, oPtr->filters) {
	    Tcl_DecrRefCount(filterObj);
	}
    }

    if (numFilters == 0) {
	/*
	 * No list of filters was supplied, so we're deleting filters.
	 */

	ckfree((char *) oPtr->filters.list);
	oPtr->filters.list = NULL;
	oPtr->filters.num = 0;
    } else {
	/*
	 * We've got a list of filters, so we're creating filters.
	 */

	Tcl_Obj **filtersList;
	int size = sizeof(Tcl_Obj *) * numFilters;	/* should be size_t */

	if (oPtr->filters.num == 0) {
	    filtersList = (Tcl_Obj **) ckalloc(size);
	} else {
	    filtersList = (Tcl_Obj **)
		    ckrealloc((char *) oPtr->filters.list, size);
	}
	for (i=0 ; i<numFilters ; i++) {
	    filtersList[i] = filters[i];
	    Tcl_IncrRefCount(filters[i]);
	}
	oPtr->filters.list = filtersList;
	oPtr->filters.num = numFilters;
    }
    oPtr->epoch++;		/* Only this object can be affected. */
}

void
TclOOClassSetFilters(
    Tcl_Interp *interp,
    Class *classPtr,
    int numFilters,
    Tcl_Obj *const *filters)
{
    int i;

    if (classPtr->filters.num) {
	Tcl_Obj *filterObj;

	FOREACH(filterObj, classPtr->filters) {
	    Tcl_DecrRefCount(filterObj);
	}
    }

    if (numFilters == 0) {
	/*
	 * No list of filters was supplied, so we're deleting filters.
	 */

	ckfree((char *) classPtr->filters.list);
	classPtr->filters.list = NULL;
	classPtr->filters.num = 0;
    } else {
	/*
	 * We've got a list of filters, so we're creating filters.
	 */

	Tcl_Obj **filtersList;
	int size = sizeof(Tcl_Obj *) * numFilters;	/* should be size_t */

	if (classPtr->filters.num == 0) {
	    filtersList = (Tcl_Obj **) ckalloc(size);
	} else {
	    filtersList = (Tcl_Obj **)
		    ckrealloc((char *) classPtr->filters.list, size);
	}
	for (i=0 ; i<numFilters ; i++) {
	    filtersList[i] = filters[i];
	    Tcl_IncrRefCount(filters[i]);
	}
	classPtr->filters.list = filtersList;
	classPtr->filters.num = numFilters;
    }

    /*
     * There may be many objects affected, so bump the global epoch.
     */

    BumpGlobalEpoch(interp, classPtr);
}

int
TclOODefineFilterObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const *objv)
{
    int isSelfFilter = (clientData != NULL);
    Object *oPtr = (Object *) TclOOGetDefineCmdContext(interp);

    if (oPtr == NULL) {
	return TCL_ERROR;
    }
    if (!isSelfFilter && !oPtr->classPtr) {
	Tcl_AppendResult(interp, "attempt to misuse API", NULL);
	return TCL_ERROR;
    }

    if (!isSelfFilter) {
	TclOOClassSetFilters(interp, oPtr->classPtr, objc-1, objv+1);
    } else {
	TclOOObjectSetFilters(oPtr, objc-1, objv+1);
    }
    return TCL_OK;
}

int
TclOODefineForwardObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const *objv)
{
    int isSelfForward = (clientData != NULL);
    Object *oPtr;
    Method *mPtr;
    int isPublic;
    Tcl_Obj *prefixObj;

    if (objc < 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "name cmdName ?arg ...?");
	return TCL_ERROR;
    }

    oPtr = (Object *) TclOOGetDefineCmdContext(interp);
    if (oPtr == NULL) {
	return TCL_ERROR;
    }
    if (!isSelfForward && !oPtr->classPtr) {
	Tcl_AppendResult(interp, "attempt to misuse API", NULL);
	return TCL_ERROR;
    }
    isPublic = Tcl_StringMatch(TclGetString(objv[1]), "[a-z]*")
	    ? PUBLIC_METHOD : 0;

    /*
     * Create the method structure.
     */

    prefixObj = Tcl_NewListObj(objc-2, objv+2);
    if (isSelfForward) {
	mPtr = TclOONewForwardInstanceMethod(interp, oPtr, isPublic, objv[1],
		prefixObj);
    } else {
	mPtr = TclOONewForwardMethod(interp, oPtr->classPtr, isPublic,
		objv[1], prefixObj);
    }
    if (mPtr == NULL) {
	Tcl_DecrRefCount(prefixObj);
	return TCL_ERROR;
    }
    return TCL_OK;
}

int
TclOODefineMethodObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const *objv)
{
    int isSelfMethod = (clientData != NULL);
    Object *oPtr;
    int bodyLength;

    if (objc != 4) {
	Tcl_WrongNumArgs(interp, 1, objv, "name args body");
	return TCL_ERROR;
    }

    oPtr = (Object *) TclOOGetDefineCmdContext(interp);
    if (oPtr == NULL) {
	return TCL_ERROR;
    }
    if (!isSelfMethod && !oPtr->classPtr) {
	Tcl_AppendResult(interp, "attempt to misuse API", NULL);
	return TCL_ERROR;
    }

    (void) Tcl_GetStringFromObj(objv[3], &bodyLength);
    if (bodyLength > 0) {
	/*
	 * Create the method structure.
	 */

	Method *mPtr;
	int isPublic = Tcl_StringMatch(TclGetString(objv[1]), "[a-z]*")
		? PUBLIC_METHOD : 0;

	if (isSelfMethod) {
	    mPtr = TclOONewProcInstanceMethod(interp, oPtr, isPublic, objv[1],
		    objv[2], objv[3], NULL);
	} else {
	    mPtr = TclOONewProcMethod(interp, oPtr->classPtr, isPublic,
		    objv[1], objv[2], objv[3], NULL);
	}
	if (mPtr == NULL) {
	    return TCL_ERROR;
	}
    } else {
	/*
	 * Delete the method structure from the appropriate hash table.
	 */

	Tcl_HashEntry *hPtr;

	if (isSelfMethod) {
	    hPtr = Tcl_FindHashEntry(&oPtr->methods, (char *)objv[1]);
	} else {
	    hPtr = Tcl_FindHashEntry(&oPtr->classPtr->classMethods,
		    (char *)objv[1]);
	}
	if (hPtr != NULL) {
	    Method *mPtr = (Method *) Tcl_GetHashValue(hPtr);

	    Tcl_DeleteHashEntry(hPtr);
	    TclOODeleteMethod(mPtr);
	}
    }

    return TCL_OK;
}

void
TclOOObjectSetMixins(
    Object *oPtr,
    int numMixins,
    Class *const *mixins)
{
    Class *mixinPtr;
    int i;

    if (numMixins == 0) {
	if (oPtr->mixins.num != 0) {
	    FOREACH(mixinPtr, oPtr->mixins) {
		TclOORemoveFromInstances(oPtr, mixinPtr);
	    }
	    ckfree((char *) oPtr->mixins.list);
	    oPtr->mixins.num = 0;
	}
    } else {
	if (oPtr->mixins.num != 0) {
	    FOREACH(mixinPtr, oPtr->mixins) {
		if (mixinPtr != oPtr->selfCls) {
		    TclOORemoveFromInstances(oPtr, mixinPtr);
		}
	    }
	    oPtr->mixins.list = (Class **)
		    ckrealloc((char *) oPtr->mixins.list,
		    sizeof(Class *) * numMixins);
	} else {
	    oPtr->mixins.list = (Class **)
		    ckalloc(sizeof(Class *) * numMixins);
	}
	oPtr->mixins.num = numMixins;
	memcpy(oPtr->mixins.list, mixins, sizeof(Class *) * numMixins);
	FOREACH(mixinPtr, oPtr->mixins) {
	    if (mixinPtr != oPtr->selfCls) {
		TclOOAddToInstances(oPtr, mixinPtr);
	    }
	}
    }
    oPtr->epoch++;
}

void
TclOOClassSetMixins(
    Tcl_Interp *interp,
    Class *classPtr,
    int numMixins,
    Class *const *mixins)
{
    Class *mixinPtr;
    int i;

    if (numMixins == 0) {
	if (classPtr->mixins.num != 0) {
	    FOREACH(mixinPtr, classPtr->mixins) {
		TclOORemoveFromMixinSubs(classPtr, mixinPtr);
	    }
	    ckfree((char *) classPtr->mixins.list);
	    classPtr->mixins.num = 0;
	}
    } else {
	if (classPtr->mixins.num != 0) {
	    FOREACH(mixinPtr, classPtr->mixins) {
		TclOORemoveFromMixinSubs(classPtr, mixinPtr);
	    }
	    classPtr->mixins.list = (Class **)
		    ckrealloc((char *) classPtr->mixins.list,
		    sizeof(Class *) * numMixins);
	} else {
	    classPtr->mixins.list = (Class **)
		    ckalloc(sizeof(Class *) * numMixins);
	}
	classPtr->mixins.num = numMixins;
	memcpy(classPtr->mixins.list, mixins, sizeof(Class *) * numMixins);
	FOREACH(mixinPtr, classPtr->mixins) {
	    TclOOAddToMixinSubs(classPtr, mixinPtr);
	}
    }
    BumpGlobalEpoch(interp, classPtr);
}

int
TclOODefineMixinObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    const int objc,
    Tcl_Obj *const *objv)
{
    int isSelfMixin = (clientData != NULL);
    Object *oPtr = (Object *) TclOOGetDefineCmdContext(interp);
    Class **mixins;
    int i;

    if (oPtr == NULL) {
	return TCL_ERROR;
    }
    if (!isSelfMixin && !oPtr->classPtr) {
	Tcl_AppendResult(interp, "attempt to misuse API", NULL);
	return TCL_ERROR;
    }
    mixins = TclStackAlloc(interp, sizeof(Class *) * (objc-1));

    for (i=1 ; i<objc ; i++) {
	Object *o2Ptr;

	o2Ptr = (Object *) Tcl_GetObjectFromObj(interp, objv[i]);
	if (o2Ptr == NULL) {
	    goto freeAndError;
	}
	if (o2Ptr->classPtr == NULL) {
	    Tcl_AppendResult(interp, "may only mix in classes; \"",
		    TclGetString(objv[i]), "\" is not a class", NULL);
	    goto freeAndError;
	}
	if (!isSelfMixin && TclOOIsReachable(oPtr->classPtr,o2Ptr->classPtr)){
	    Tcl_AppendResult(interp, "may not mix a class into itself", NULL);
	    goto freeAndError;
	}
	mixins[i-1] = o2Ptr->classPtr;
    }

    if (isSelfMixin) {
	TclOOObjectSetMixins(oPtr, objc-1, mixins);
    } else {
	TclOOClassSetMixins(interp, oPtr->classPtr, objc-1, mixins);
    }

    TclStackFree(interp, mixins);
    return TCL_OK;

  freeAndError:
    TclStackFree(interp, mixins);
    return TCL_ERROR;
}

int
TclOODefineSelfClassObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const *objv)
{
    Object *oPtr, *o2Ptr;
    Foundation *fPtr = TclOOGetFoundation(interp);

    /*
     * Parse the context to get the object to operate on.
     */

    oPtr = (Object *) TclOOGetDefineCmdContext(interp);
    if (oPtr == NULL) {
	return TCL_ERROR;
    }
    if (oPtr == fPtr->objectCls->thisPtr) {
	Tcl_AppendResult(interp,
		"may not modify the class of the root object", NULL);
	return TCL_ERROR;
    }
    if (oPtr == fPtr->classCls->thisPtr) {
	Tcl_AppendResult(interp,
		"may not modify the class of the class of classes", NULL);
	return TCL_ERROR;
    }

    /*
     * Parse the argument to get the class to set the object's class to.
     */

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "className");
	return TCL_ERROR;
    }
    o2Ptr = (Object *) Tcl_GetObjectFromObj(interp, objv[1]);
    if (o2Ptr == NULL) {
	return TCL_ERROR;
    }
    if (o2Ptr->classPtr == NULL) {
	Tcl_AppendResult(interp, "the class of an object must be a class",
		NULL);
	return TCL_ERROR;
    }

    /*
     * Apply semantic checks. In particular, classes and non-classes are not
     * interchangable (too complicated to do the conversion!) so we must
     * produce an error if any attempt is made to swap from one to the other.
     */

    if ((oPtr->classPtr == NULL) == TclOOIsReachable(fPtr->classCls,
	    o2Ptr->classPtr)) {
	Tcl_AppendResult(interp, "may not change a ",
		(oPtr->classPtr==NULL ? "non-" : ""), "class object into a ",
		(oPtr->classPtr==NULL ? "" : "non-"), "class object", NULL);
	return TCL_ERROR;
    }

    /*
     * Set the object's class.
     */

    if (oPtr->selfCls != o2Ptr->classPtr) {
	TclOORemoveFromInstances(oPtr, oPtr->selfCls);
	oPtr->selfCls = o2Ptr->classPtr;
	TclOOAddToInstances(oPtr, oPtr->selfCls);
	if (oPtr->classPtr != NULL) {
	    BumpGlobalEpoch(interp, oPtr->classPtr);
	} else {
	    oPtr->epoch++;
	}
    }
    return TCL_OK;
}

int
TclOODefineSuperclassObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const *objv)
{
    Object *oPtr, *o2Ptr;
    Foundation *fPtr = TclOOGetFoundation(interp);
    Class **superclasses, *superPtr;
    int i, j;

    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "className ?className ...?");
	return TCL_ERROR;
    }

    /*
     * Get the class to operate on.
     */

    oPtr = (Object *) TclOOGetDefineCmdContext(interp);
    if (oPtr == NULL) {
	return TCL_ERROR;
    }
    if (oPtr->classPtr == NULL) {
	Tcl_AppendResult(interp, "only classes may have superclasses defined",
		NULL);
	return TCL_ERROR;
    }
    if (oPtr == fPtr->objectCls->thisPtr) {
	Tcl_AppendResult(interp,
		"may not modify the superclass of the root object", NULL);
	return TCL_ERROR;
    }

    /*
     * Allocate some working space.
     */

    superclasses = (Class **) ckalloc(sizeof(Class *) * (objc-1));

    /*
     * Parse the arguments to get the class to use as superclasses.
     */

    for (i=0 ; i<objc-1 ; i++) {
	o2Ptr = (Object *) Tcl_GetObjectFromObj(interp, objv[i+1]);
	if (o2Ptr == NULL) {
	    goto failedAfterAlloc;
	}
	if (o2Ptr->classPtr == NULL) {
	    Tcl_AppendResult(interp, "only a class can be a superclass",NULL);
	    goto failedAfterAlloc;
	}
	for (j=0 ; j<i ; j++) {
	    if (superclasses[j] == o2Ptr->classPtr) {
		Tcl_AppendResult(interp,
			"class should only be a direct superclass once",NULL);
		goto failedAfterAlloc;
	    }
	}
	if (TclOOIsReachable(oPtr->classPtr, o2Ptr->classPtr)) {
	    Tcl_AppendResult(interp,
		    "attempt to form circular dependency graph", NULL);
	failedAfterAlloc:
	    ckfree((char *) superclasses);
	    return TCL_ERROR;
	}
	superclasses[i] = o2Ptr->classPtr;
    }

    /*
     * Install the list of superclasses into the class. Note that this also
     * involves splicing the class out of the superclasses' subclass list that
     * it used to be a member of and splicing it into the new superclasses'
     * subclass list.
     */

    if (oPtr->classPtr->superclasses.num != 0) {
	FOREACH(superPtr, oPtr->classPtr->superclasses) {
	    TclOORemoveFromSubclasses(oPtr->classPtr, superPtr);
	}
	ckfree((char *) oPtr->classPtr->superclasses.list);
    }
    oPtr->classPtr->superclasses.list = superclasses;
    oPtr->classPtr->superclasses.num = objc-1;
    FOREACH(superPtr, oPtr->classPtr->superclasses) {
	TclOOAddToSubclasses(oPtr->classPtr, superPtr);
    }
    BumpGlobalEpoch(interp, oPtr->classPtr);

    return TCL_OK;
}

int
TclOODefineUnexportObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const *objv)
{
    int isSelfUnexport = (clientData != NULL);
    Object *oPtr;
    Method *mPtr;
    Tcl_HashEntry *hPtr;
    Class *clsPtr;
    int i, isNew;

    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name ?name ...?");
	return TCL_ERROR;
    }

    oPtr = (Object *) TclOOGetDefineCmdContext(interp);
    if (oPtr == NULL) {
	return TCL_ERROR;
    }
    clsPtr = oPtr->classPtr;
    if (!isSelfUnexport && !clsPtr) {
	Tcl_AppendResult(interp, "attempt to misuse API", NULL);
	return TCL_ERROR;
    }

    for (i=1 ; i<objc ; i++) {
	if (isSelfUnexport) {
	    hPtr = Tcl_CreateHashEntry(&oPtr->methods, (char *) objv[i],
		    &isNew);
	} else {
	    hPtr = Tcl_CreateHashEntry(&clsPtr->classMethods, (char*) objv[i],
		    &isNew);
	}

	if (isNew) {
	    mPtr = (Method *) ckalloc(sizeof(Method));
	    memset(mPtr, 0, sizeof(Method));
	    Tcl_SetHashValue(hPtr, mPtr);
	} else {
	    mPtr = Tcl_GetHashValue(hPtr);
	}
	mPtr->flags &= ~PUBLIC_METHOD;
    }
    if (isSelfUnexport) {
	oPtr->epoch++;
    } else {
	BumpGlobalEpoch(interp, clsPtr);
    }
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
