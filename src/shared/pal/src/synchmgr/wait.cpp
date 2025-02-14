// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

/*++



Module Name:

    wait.cpp

Abstract:

    Implementation of waiting functions as described in
    the WIN32 API

Revision History:



--*/

#include "pal/thread.hpp"
#include "pal/synchobjects.hpp"
#include "pal/handlemgr.hpp"
#include "pal/mutex.hpp"
#include "pal/malloc.hpp"
#include "pal/dbgmsg.h"

SET_DEFAULT_DEBUG_CHANNEL(SYNC);

#define MAXIMUM_STACK_WAITOBJ_ARRAY_SIZE (MAXIMUM_WAIT_OBJECTS / 4)

using namespace CorUnix;

static PalObjectTypeId sg_rgWaitObjectsIds[] =
    {
        otiProcess,
        otiThread
    };
static CAllowedObjectTypes sg_aotWaitObject(sg_rgWaitObjectsIds,
    sizeof(sg_rgWaitObjectsIds)/sizeof(sg_rgWaitObjectsIds[0]));

/*++
Function:
  WaitForSingleObject

See MSDN doc.
--*/
DWORD
PALAPI
WaitForSingleObject(IN HANDLE hHandle,
                    IN DWORD dwMilliseconds)
{
    DWORD dwRet;

    PERF_ENTRY(WaitForSingleObject);
    ENTRY("WaitForSingleObject(hHandle=%p, dwMilliseconds=%u)\n",
          hHandle, dwMilliseconds);

    CPalThread * pThread = InternalGetCurrentThread();

    dwRet = InternalWaitForMultipleObjectsEx(pThread, 1, &hHandle, FALSE,
                                             dwMilliseconds, FALSE);

    LOGEXIT("WaitForSingleObject returns DWORD %u\n", dwRet);
    PERF_EXIT(WaitForSingleObject);
    return dwRet;
}

DWORD CorUnix::InternalWaitForMultipleObjectsEx(
    CPalThread * pThread,
    DWORD nCount,
    CONST HANDLE *lpHandles,
    BOOL bWaitAll,
    DWORD dwMilliseconds,
    BOOL bAlertable,
    BOOL bPrioritize)
{
    DWORD dwRet = WAIT_FAILED;
    PAL_ERROR palErr = NO_ERROR;
    int i, iSignaledObjCount, iSignaledObjIndex = -1;
    bool fWAll = (bool)bWaitAll, fNeedToBlock  = false;
    bool fAbandoned = false;
    WaitType wtWaitType;

    IPalObject            * pIPalObjStackArray[MAXIMUM_STACK_WAITOBJ_ARRAY_SIZE] = { NULL };
    ISynchWaitController  * pISyncStackArray[MAXIMUM_STACK_WAITOBJ_ARRAY_SIZE] = { NULL };
    IPalObject           ** ppIPalObjs = pIPalObjStackArray;
    ISynchWaitController ** ppISyncWaitCtrlrs = pISyncStackArray;

    if ((nCount == 0) || (nCount > MAXIMUM_WAIT_OBJECTS))
    {
        ppIPalObjs = NULL;        // make delete at the end safe
        ppISyncWaitCtrlrs = NULL; // make delete at the end safe
        ERROR("Invalid object count=%d [range: 1 to %d]\n",
               nCount, MAXIMUM_WAIT_OBJECTS)
        pThread->SetLastError(ERROR_INVALID_PARAMETER);
        goto WFMOExIntExit;
    }
    else if (nCount == 1)
    {
        fWAll = false;  // makes no difference when nCount is 1
        wtWaitType = SingleObject;
    }
    else
    {
        wtWaitType = fWAll ? MultipleObjectsWaitAll : MultipleObjectsWaitOne;
        if (nCount > MAXIMUM_STACK_WAITOBJ_ARRAY_SIZE)
        {
            ppIPalObjs = InternalNewArray<IPalObject*>(nCount);
            ppISyncWaitCtrlrs = InternalNewArray<ISynchWaitController*>(nCount);
            if ((NULL == ppIPalObjs) || (NULL == ppISyncWaitCtrlrs))
            {
                ERROR("Out of memory allocating internal structures\n");
                pThread->SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                goto WFMOExIntExit;
            }
        }
    }

    palErr = g_pObjectManager->ReferenceMultipleObjectsByHandleArray(pThread,
                                                                     (VOID **)lpHandles,
                                                                     nCount,
                                                                     &sg_aotWaitObject,
                                                                     ppIPalObjs);
    if (NO_ERROR != palErr)
    {
        ERROR("Unable to obtain object for some or all of the handles [error=%u]\n",
              palErr);
        if (palErr == ERROR_INVALID_HANDLE)
            pThread->SetLastError(ERROR_INVALID_HANDLE);
        else
            pThread->SetLastError(ERROR_INTERNAL_ERROR);
        goto WFMOExIntExit;
    }

    if (nCount > 1)
    {
        ERROR("Attempt to wait for any or all handles including a cross-process sync object", ERROR_NOT_SUPPORTED);
        pThread->SetLastError(ERROR_NOT_SUPPORTED);
        goto WFMOExIntCleanup;
    }

    if (fWAll)
    {
        // For a wait-all operation, check for duplicate wait objects in the array. This just uses a brute-force O(n^2)
        // algorithm, but since MAXIMUM_WAIT_OBJECTS is small, the worst case is not so bad, and the average case would involve
        // significantly fewer items.
        for (DWORD i = 0; i < nCount - 1; ++i)
        {
            IPalObject *const objectToCheck = ppIPalObjs[i];
            for (DWORD j = i + 1; j < nCount; ++j)
            {
                if (ppIPalObjs[j] == objectToCheck)
                {
                    ERROR("Duplicate handle provided for a wait-all operation [error=%u]\n", ERROR_INVALID_PARAMETER);
                    pThread->SetLastError(ERROR_INVALID_PARAMETER);
                    goto WFMOExIntCleanup;
                }
            }
        }
    }

    palErr = g_pSynchronizationManager->GetSynchWaitControllersForObjects(
        pThread, ppIPalObjs, nCount, ppISyncWaitCtrlrs);
    if (NO_ERROR != palErr)
    {
        ERROR("Unable to obtain ISynchWaitController interface for some or all "
              "of the objects [error=%u]\n", palErr);
        pThread->SetLastError(ERROR_INTERNAL_ERROR);
        goto WFMOExIntCleanup;
    }

    if (bAlertable)
    {
        pThread->SetLastError(ERROR_INTERNAL_ERROR);
        dwRet = WAIT_FAILED;
        goto WFMOExIntCleanup;
    }

    iSignaledObjCount = 0;
    iSignaledObjIndex = -1;
    for (i=0;i<(int)nCount;i++)
    {
        bool fValue, fWaitObjectAbandoned = false;
        palErr = ppISyncWaitCtrlrs[i]->CanThreadWaitWithoutBlocking(&fValue, &fWaitObjectAbandoned);
        if (NO_ERROR != palErr)
        {
            ERROR("ISynchWaitController::CanThreadWaitWithoutBlocking() failed for "
                  "%d-th object [handle=%p error=%u]\n", i, lpHandles[i], palErr);
            pThread->SetLastError(ERROR_INTERNAL_ERROR);
            goto WFMOExIntReleaseControllers;
        }
        if (fWaitObjectAbandoned)
        {
            fAbandoned = true;
        }
        if (fValue)
        {
            iSignaledObjCount++;
            iSignaledObjIndex = i;
            if (!fWAll)
                break;
        }
    }

    fNeedToBlock = (iSignaledObjCount == 0) || (fWAll && (iSignaledObjCount < (int)nCount));
    if (!fNeedToBlock)
    {
        // At least one object signaled, or bWaitAll==TRUE and all object signaled.
        // No need to wait, let's unsignal the object(s) and return without blocking
        int iStartIdx, iEndIdx;

        if (fWAll)
        {
            iStartIdx = 0;
            iEndIdx = nCount;
        }
        else
        {
            iStartIdx = iSignaledObjIndex;
            iEndIdx = iStartIdx + 1;
        }

        // Unsignal objects
        if( iStartIdx < 0 )
        {
            ERROR("Buffer underflow due to iStartIdx < 0");
            pThread->SetLastError(ERROR_INTERNAL_ERROR);
            dwRet = WAIT_FAILED;
            goto WFMOExIntCleanup;
        }
        for (i = iStartIdx; i < iEndIdx; i++)
        {
            palErr = ppISyncWaitCtrlrs[i]->ReleaseWaitingThreadWithoutBlocking();
            if (NO_ERROR != palErr)
            {
                ERROR("ReleaseWaitingThreadWithoutBlocking() failed for %d-th "
                      "object [handle=%p error=%u]\n",
                      i, lpHandles[i], palErr);
                pThread->SetLastError(palErr);
                goto WFMOExIntReleaseControllers;
            }
        }

        dwRet = (fAbandoned ? WAIT_ABANDONED_0 : WAIT_OBJECT_0);
    }
    else if (0 == dwMilliseconds)
    {
        // Not enough objects signaled, but timeout is zero: no actual wait
        dwRet = WAIT_TIMEOUT;
        fNeedToBlock = false;
    }
    else
    {
        // Register the thread for waiting on all objects
        for (i=0;i<(int)nCount;i++)
        {
            palErr = ppISyncWaitCtrlrs[i]->RegisterWaitingThread(
                                                        wtWaitType,
                                                        i,
                                                        (TRUE == bAlertable),
                                                        bPrioritize != FALSE);
            if (NO_ERROR != palErr)
            {
                ERROR("RegisterWaitingThread() failed for %d-th object "
                      "[handle=%p error=%u]\n", i, lpHandles[i], palErr);
                pThread->SetLastError(palErr);
                goto WFMOExIntReleaseControllers;
            }
        }
    }

WFMOExIntReleaseControllers:
    // Release all controllers before going to sleep
    for (i = 0; i < (int)nCount; i++)
    {
        ppISyncWaitCtrlrs[i]->ReleaseController();
        ppISyncWaitCtrlrs[i] = NULL;
    }
    if (NO_ERROR != palErr)
        goto WFMOExIntCleanup;

    if (fNeedToBlock)
    {
        ThreadWakeupReason twrWakeupReason;

        //
        // Going to sleep
        //
        palErr = g_pSynchronizationManager->BlockThread(pThread,
                                                        dwMilliseconds,
                                                        (TRUE == bAlertable),
                                                        false,
                                                        &twrWakeupReason,
                                                        (DWORD *)&iSignaledObjIndex);
        //
        // Awakened
        //
        if (NO_ERROR != palErr)
        {
            ERROR("IPalSynchronizationManager::BlockThread failed for thread "
                  "pThread=%p [error=%u]\n", pThread, palErr);
            pThread->SetLastError(palErr);
            goto WFMOExIntCleanup;
        }
        switch (twrWakeupReason)
        {
        case WaitSucceeded:
            dwRet = WAIT_OBJECT_0; // offset added later
            break;
        case MutexAbondoned:
            dwRet =  WAIT_ABANDONED_0; // offset added later
            break;
        case WaitTimeout:
            dwRet = WAIT_TIMEOUT;
            break;
        case WaitFailed:
        default:
            ERROR("Thread %p awakened with some failure\n", pThread);
            dwRet = WAIT_FAILED;
            break;
        }
    }

    if (!fWAll && ((WAIT_OBJECT_0 == dwRet) || (WAIT_ABANDONED_0 == dwRet)))
    {
        _ASSERT_MSG(0 <= iSignaledObjIndex,
                    "Failed to identify signaled/abandoned object\n");
        _ASSERT_MSG(iSignaledObjIndex >= 0 && nCount > static_cast<DWORD>(iSignaledObjIndex),
                    "SignaledObjIndex object out of range "
                    "[index=%d obj_count=%u\n",
                    iSignaledObjCount, nCount);

        if (iSignaledObjIndex < 0)
        {
            pThread->SetLastError(ERROR_INTERNAL_ERROR);
            dwRet = WAIT_FAILED;
            goto WFMOExIntCleanup;
        }
        dwRet += iSignaledObjIndex;
    }

WFMOExIntCleanup:
    for (i = 0; i < (int)nCount; i++)
    {
        ppIPalObjs[i]->ReleaseReference(pThread);
        ppIPalObjs[i] = NULL;
    }

WFMOExIntExit:
    if (nCount > MAXIMUM_STACK_WAITOBJ_ARRAY_SIZE)
    {
        InternalDeleteArray(ppIPalObjs);
        InternalDeleteArray(ppISyncWaitCtrlrs);
    }

    return dwRet;
}
