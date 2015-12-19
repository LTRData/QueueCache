/*
 * SPDX-License-Identifier: MS-LPL
 *
 * Portions derived from Microsoft's DiskPerf sample.
 * Copyright (C) Microsoft Corporation, 1991 - 1999
 * QueueCache modifications: Copyright (c) 2015-2026 Olof Lagerkvist.
 *
 * Distributed under the Microsoft Limited Public License (MS-LPL).
 * See ../LICENSES/MS-LPL.txt and ../THIRD_PARTY_NOTICES.md.
 */

/*
* QCache.h
*
* Based on LoopBack Filter Driver by Mayur Thigale.
* Refer for loopback driver http://www.codeproject.com/KB/system/loopback.aspx
*/

#pragma once

#include <Ntifs.h>
#include <ntdddisk.h>
#include <ntddscsi.h>
#include <stdarg.h>
#include <stdio.h>
#include <ntddvol.h>

#include <mountdev.h>

#include <ntstrsafe.h>

#define INITGUID

#include "qcstats.h"

//
// Number of bits to use in block size mask. For instance,
// 21 = 2 MB, 19 = 512 KB, 16 = 64 K, 12 = 4 K etc.
// The smaller block size the more non-paged pool is needed
// for the allocation table. On the other hand, smaller block
// sizes mean less space likely wasted on diff device to fill
// up complete blocks as new blocks are allocated by small
// write requests.
//
#define DIFF_BLOCK_BITS                         16

//
// Macros for easier block/offset calculation
//
#define DIFF_BLOCK_SIZE                         (1ULL << DIFF_BLOCK_BITS)
#define DIFF_BLOCK_OFFSET_MASK                  (DIFF_BLOCK_SIZE - 1)
#define DIFF_BLOCK_BASE_MASK                    (~(DIFF_BLOCK_SIZE - 1))
#define DIFF_GET_BLOCK_NUMBER(a)                ((a) >> DIFF_BLOCK_BITS)
#define DIFF_GET_NUMBER_OF_BLOCKS(a)            (((a) + DIFF_BLOCK_OFFSET_MASK) >> DIFF_BLOCK_BITS)
#define DIFF_GET_BLOCK_OFFSET(a)                ((ULONG)((a) & DIFF_BLOCK_OFFSET_MASK))
#define DIFF_GET_BLOCK_BASE_FROM_ABS_OFFSET(a)  ((a) & DIFF_BLOCK_BASE_MASK)

#define DIFF_BLOCK_UNALLOCATED                  (0x00000000UL)

#define SECTOR_BITS                             9
#define SECTOR_SIZE                             (1L << SECTOR_BITS)

#define IDLE_TRIM_BLOCKS_INTERVAL               32

#ifdef _WIN64
#define InterlockedAddPtr InterlockedAdd64
#else
#define InterlockedAddPtr InterlockedAdd
#endif

#ifdef POOL_TAGGING
#ifdef ExAllocatePool
#undef ExAllocatePool
#endif
#define ExAllocatePool(a,b) ExAllocatePoolWithTag(a,b,POOL_TAG)
#ifdef ExFreePool
#undef ExFreePool
#endif
#define ExFreePool(a) ExFreePoolWithTag(a,POOL_TAG)
#endif

inline void * __CRTDECL operator new(size_t Size)
{
    void * result = ExAllocatePoolWithTag(NonPagedPool, Size, POOL_TAG);

    if (result != NULL)
    {
        RtlZeroMemory(result, Size);
    }

    return result;
}

inline void * __CRTDECL operator new(size_t Size, UCHAR FillByte)
{
    void * result = ExAllocatePoolWithTag(NonPagedPool, Size, POOL_TAG);

    if (result != NULL)
    {
        RtlFillMemory(result, Size, FillByte);
    }

    return result;
}

inline void __CRTDECL operator delete(void * Ptr)
{
    if (Ptr != NULL)
    {
        ExFreePoolWithTag(Ptr, POOL_TAG);
    }
}

template<typename T> class WPoolMem
{
protected:
    T *ptr;
    SIZE_T bytecount;

    explicit WPoolMem(T *pBlk, SIZE_T AllocationSize)
        : ptr(pBlk),
        bytecount(pBlk != NULL ? AllocationSize : 0) { }

public:
    operator bool()
    {
        return ptr != NULL;
    }

    bool operator!()
    {
        return ptr == NULL;
    }

    operator T*()
    {
        return ptr;
    }

    T* operator ->()
    {
        return ptr;
    }

    T& operator[](int i)
    {
        return ptr[i];
    }

    T* operator+(int i)
    {
        return ptr + i;
    }

    T* operator-(int i)
    {
        return ptr - i;
    }

    T* operator =(T *pBlk)
    {
        Free();
        return ptr = pBlk;
    }

    SIZE_T Count() const
    {
        return GetSize() / sizeof(T);
    }

    SIZE_T GetSize() const
    {
        return ptr != NULL ? bytecount : 0;
    }

    void Zero() const
    {
        RtlZeroMemory(ptr, GetSize());
    }

    void Fill(int value) const
    {
        RtlFillMemory(ptr, GetSize(), fill);
    }

    void Free()
    {
        if (ptr != NULL)
        {
            ExFreePool(ptr);
            ptr = NULL;
        }
    }

    void Clear()
    {
        if ((ptr != NULL) && (bytecount > 0))
        {
            RtlZeroMemory(ptr, bytecount);
        }
    }

    T* Abandon()
    {
        T* ab_ptr = ptr;
        ptr = NULL;
        bytecount = 0;
        return ab_ptr;
    }

    WPoolMem() :
        ptr(NULL),
        bytecount(0) { }

    ~WPoolMem()
    {
        Free();
    }
};

template<typename T> class WPagedPoolMem : public WPoolMem < T >
{
public:
    explicit WPagedPoolMem(SIZE_T AllocateSize)
        : WPoolMem((T*)ExAllocatePool(PagedPool, AllocateSize), AllocateSize)
    {
    }
};

template<typename T> class WNonPagedPoolMem : public WPoolMem < T >
{
public:
    explicit WNonPagedPoolMem(SIZE_T AllocateSize)
        : WPoolMem((T*)ExAllocatePool(NonPagedPool, AllocateSize), AllocateSize)
    {
    }
};

class WHandle
{
private:
    HANDLE h;

public:
    operator bool()
    {
        return h != NULL;
    }

    bool operator !()
    {
        return h == NULL;
    }

    operator HANDLE()
    {
        return h;
    }

    void Close()
    {
        if (h != NULL)
        {
            ZwClose(h);
            h = NULL;
        }
    }

    WHandle() :
        h(NULL) { }

    explicit WHandle(HANDLE h) :
        h(h) { }

    ~WHandle()
    {
        Close();
    }
};

//
// Device Extension
//

typedef struct _DEVICE_EXTENSION
{

    //
    // Back pointer to device object
    //

    PDEVICE_OBJECT DeviceObject;

    //
    // Target Device Object
    //

    PDEVICE_OBJECT TargetDeviceObject;

    //
    // Physical device object
    //
    PDEVICE_OBJECT PhysicalDeviceObject;

    //
    //
    //
    DEVICE_STATISTICS Statistics;

    //
    //
    //
    HANDLE WorkerThread;

    //
    //
    //
    bool ShutdownThread;

    //
    // RemoveLock prevents removal of a device while it is busy.
    //
    IO_REMOVE_LOCK RemoveLock;

    //
    // 
    //
    LIST_ENTRY WriteQueue;
    
    //
    // 
    //
    KSPIN_LOCK WriteQueueLock;

    //
    //
    //
    KEVENT WriteQueueEvent;

    //
    // 
    //
    KGUARDED_MUTEX InitializationMutex;

    //
    // 
    //
    KEVENT InitializationEvent;

    //
    // must synchronize paging path notifications
    //
    KEVENT PagingPathCountEvent;

    //
    // must synchronize paging path notifications
    //
    KGUARDED_MUTEX PagingPathCountMutex;

} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

#define DEVICE_EXTENSION_SIZE sizeof(DEVICE_EXTENSION)

typedef class WRITE_QUEUE_ITEM
{
public:

    //
    //
    //
    PIO_REMOVE_LOCK RemoveLock;

    //
    //
    //
    UCHAR MajorFunction;

    //
    //
    //
    LARGE_INTEGER Offset;

    //
    //
    //
    ULONG Length;

    //
    //
    //
    PUCHAR Buffer;

    //
    //
    //
    PIRP Irp;

    //
    //
    //
    LIST_ENTRY ListEntry;

    ~WRITE_QUEUE_ITEM()
    {
        delete[] Buffer;
    }

} *PWRITE_QUEUE_ITEM;

FORCEINLINE
VOID
QCacheFreeIrpWithMdls(IN PIRP Irp)
{
    if (Irp->MdlAddress != NULL)
    {
        PMDL mdl;
        PMDL nextMdl;
        for (mdl = Irp->MdlAddress; mdl != NULL; mdl = nextMdl)
        {
            nextMdl = mdl->Next;

            if (mdl->MdlFlags & MDL_PAGES_LOCKED)
            {
                MmUnlockPages(mdl);
            }

            IoFreeMdl(mdl);
        }
        Irp->MdlAddress = NULL;
    }

    IoFreeIrp(Irp);
}

typedef class SCATTERED_IRP
{
    PIRP OriginalIrp;
    
    PDEVICE_OBJECT OriginalDeviceObject;

    volatile LONG ScatterCount;

    volatile NTSTATUS LastFailedStatus;

    volatile LONG_PTR BytesCompleted;

    PIO_REMOVE_LOCK RemoveLock;

    PUCHAR SystemBuffer;

    PUCHAR AllocatedBuffer;

    bool CopyBack;

    static IO_COMPLETION_ROUTINE IrpCompletionRoutine;

    SCATTERED_IRP()
    {
    }

    ~SCATTERED_IRP()
    {
        OriginalIrp->IoStatus.Status = LastFailedStatus;
        if (NT_SUCCESS(LastFailedStatus))
        {
            if (CopyBack)
            {
                RtlCopyMemory(SystemBuffer, AllocatedBuffer,
                    BytesCompleted);
            }

            OriginalIrp->IoStatus.Information = BytesCompleted;
        }
        else
        {
            KdPrint(("QCache: Lower level I/O failed: 0x%X\n",
                LastFailedStatus));

            KdBreakPoint();

            OriginalIrp->IoStatus.Information = 0;
        }

        IoCompleteRequest(OriginalIrp, IO_NO_INCREMENT);

        IoReleaseRemoveLock(RemoveLock, OriginalIrp);

        delete[] AllocatedBuffer;
    }

public:
    void Complete()
    {
        auto scatter_items = InterlockedDecrement(&ScatterCount);
        if (scatter_items == 0)
        {
            delete this;
        }
    }

    static NTSTATUS Create(
        SCATTERED_IRP **Object,
        PDEVICE_OBJECT OriginalDeviceObject,
        PIRP OriginalIrp,
        PIO_REMOVE_LOCK RemoveLock,
        PUCHAR SystemBuffer = NULL)
    {
        //
        // Acquire the remove lock so that device will not be removed while
        // processing original irp.
        //
        auto status = IoAcquireRemoveLock(RemoveLock, OriginalIrp);
        if (!NT_SUCCESS(status))
        {
            DbgPrint("QCache: Remove lock failed Irp type %i\n",
                IoGetCurrentIrpStackLocation(OriginalIrp)->MajorFunction);

            return status;
        }

        *Object = new(0) SCATTERED_IRP;

        if (*Object == NULL)
        {
            DbgPrint("QCache: Memory alloacation error.\n");

            IoReleaseRemoveLock(RemoveLock, OriginalIrp);

            return STATUS_INSUFFICIENT_RESOURCES;
        }

        (*Object)->OriginalIrp = OriginalIrp;
        (*Object)->OriginalDeviceObject = OriginalDeviceObject;
        (*Object)->RemoveLock = RemoveLock;
        (*Object)->ScatterCount = 1;
        (*Object)->SystemBuffer = SystemBuffer;

        IoMarkIrpPending(OriginalIrp);

        return STATUS_SUCCESS;
    }

    PIRP BuildIrp(
        UCHAR MajorFunction,
        PDEVICE_OBJECT DeviceObject,
        ULONG OriginalIrpOffset,
        ULONG BytesThisIrp,
        PLARGE_INTEGER LowerDeviceOffset);

} *PSCATTERED_IRP;

//
// Function declarations
//

#ifndef _Dispatch_type_
#define _Dispatch_type_(x)
#endif

extern "C"
{
    DRIVER_INITIALIZE DriverEntry;

    DRIVER_ADD_DEVICE QCacheAddDevice;

    DRIVER_DISPATCH QCacheForwardIrpSynchronous;

    _Dispatch_type_(IRP_MJ_PNP) DRIVER_DISPATCH QCachePnp;

    DRIVER_DISPATCH QCacheSendToNextDriver;

    _Dispatch_type_(IRP_MJ_CREATE) DRIVER_DISPATCH QCacheCreate;

    _Dispatch_type_(IRP_MJ_CLEANUP) DRIVER_DISPATCH QCacheCleanup;

    _Dispatch_type_(IRP_MJ_CLOSE) DRIVER_DISPATCH QCacheClose;

    _Dispatch_type_(IRP_MJ_READ) DRIVER_DISPATCH QCacheRead;

    _Dispatch_type_(IRP_MJ_WRITE) DRIVER_DISPATCH QCacheWrite;

    _Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
        DRIVER_DISPATCH QCacheDeviceControl;

    _Dispatch_type_(IRP_MJ_SHUTDOWN)
        DRIVER_DISPATCH QCacheShutdown;

    _Dispatch_type_(IRP_MJ_FLUSH_BUFFERS)
        DRIVER_DISPATCH QCacheFlushBuffers;

    DRIVER_DISPATCH QCacheTrim;
    DRIVER_DISPATCH QCacheIgnore;

    DRIVER_DISPATCH QCacheStartDevice;
    DRIVER_DISPATCH QCacheRemoveDevice;
    DRIVER_DISPATCH QCacheDeviceUsageNotification;

    IO_COMPLETION_ROUTINE QCacheSynchronousIrpCompletion;

    IO_COMPLETION_ROUTINE QCacheWriteQueueIrpCompletion;

    DRIVER_UNLOAD QCacheUnload;

    KSTART_ROUTINE QCacheDeviceWorkerThread;

    VOID
        QCacheDeferredIrp(
            PDEVICE_EXTENSION DeviceExtension,
            PWRITE_QUEUE_ITEM Item);

    VOID
        QCacheLogError(IN PDEVICE_OBJECT DeviceObject,
            IN ULONG UniqueId,
            IN NTSTATUS ErrorCode, IN NTSTATUS Status);

    VOID
        QCacheSyncFilterWithTarget(IN PDEVICE_OBJECT FilterDevice,
            IN PDEVICE_OBJECT TargetDevice);

    VOID
        QCacheCleanupDevice(IN PDEVICE_EXTENSION DeviceExtension);

    NTSTATUS
        QCacheQueueIrp(IN PDEVICE_EXTENSION DeviceExtension,
            IN PIRP Irp);

    NTSTATUS
        QCacheDeferIrp(IN PDEVICE_EXTENSION DeviceExtension,
            IN PIRP Irp);

    NTSTATUS
        QCacheSynchronousDeviceControl(
            IN PDEVICE_OBJECT DeviceObject,
            IN PFILE_OBJECT FileObject,
            IN UCHAR MajorFunction,
            IN ULONG IoControlCode,
            IN OUT PVOID SystemBuffer = NULL,
            IN ULONG InputBufferLength = 0,
            IN ULONG OutputBufferLength = 0,
            OUT PIO_STATUS_BLOCK IoStatus = NULL);

    NTSTATUS
        QCacheSynchronousReadWrite(
            IN PDEVICE_OBJECT DeviceObject,
            IN PFILE_OBJECT FileObject,
            IN UCHAR MajorFunction,
            IN OUT PVOID SystemBuffer = NULL,
            IN ULONG BufferLength = 0,
            IN PLARGE_INTEGER StartingOffset = NULL,
            OUT PIO_STATUS_BLOCK IoStatus = NULL);

    NTSTATUS
        QCacheInitializeDiffDevice(IN PDEVICE_EXTENSION DeviceExtension);

    FORCEINLINE
        PDEVICE_OBJECT
        QCacheGetLowerDeviceObjectAndDereference(
            IN PDEVICE_OBJECT DeviceObject)
    {
        auto lower_device = IoGetLowerDeviceObject(DeviceObject);
        ObDereferenceObject(DeviceObject);
        return lower_device;
    }

    extern HANDLE QCacheParametersKey;
    extern PKEVENT QCacheLowMemCondition;
    extern PKEVENT QCacheKernelHighMemoryCondition;
    extern PKEVENT QCacheKernelHighNonPagedPoolCondition;
    extern PDRIVER_OBJECT QCacheDriverObject;
    extern bool QCacheLinksCreated;

    FORCEINLINE
        VOID
        __drv_maxIRQL(DISPATCH_LEVEL)
        __drv_when(LowestAssumedIrql < DISPATCH_LEVEL, __drv_savesIRQLGlobal(QueuedSpinLock, LockHandle))
        __drv_when(LowestAssumedIrql < DISPATCH_LEVEL, __drv_setsIRQL(DISPATCH_LEVEL))
        QCacheAcquireLock_x64(__inout __deref PKSPIN_LOCK SpinLock,
            __out __deref __drv_acquiresExclusiveResource(KeQueuedSpinLockType)
            PKLOCK_QUEUE_HANDLE LockHandle,
            __in KIRQL LowestAssumedIrql)
    {
        if (LowestAssumedIrql >= DISPATCH_LEVEL)
        {
            ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

            KeAcquireInStackQueuedSpinLockAtDpcLevel(SpinLock, LockHandle);
        }
        else
        {
            KeAcquireInStackQueuedSpinLock(SpinLock, LockHandle);
        }
    }

    FORCEINLINE
        VOID
        __drv_requiresIRQL(DISPATCH_LEVEL)
        __drv_when(*LowestAssumedIrql < DISPATCH_LEVEL, __drv_restoresIRQLGlobal(QueuedSpinLock, LockHandle))
        QCacheReleaseLock_x64(
            __in __deref __drv_releasesExclusiveResource(KeQueuedSpinLockType)
            PKLOCK_QUEUE_HANDLE LockHandle,
            __inout __deref PKIRQL LowestAssumedIrql)
    {
        ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

        if (*LowestAssumedIrql >= DISPATCH_LEVEL)
        {
            KeReleaseInStackQueuedSpinLockFromDpcLevel(LockHandle);
        }
        else
        {
            KeReleaseInStackQueuedSpinLock(LockHandle);
            *LowestAssumedIrql = LockHandle->OldIrql;
        }
    }

    FORCEINLINE
        VOID
        __drv_maxIRQL(DISPATCH_LEVEL)
        __drv_when(LowestAssumedIrql < DISPATCH_LEVEL, __drv_setsIRQL(DISPATCH_LEVEL))
        QCacheAcquireLock_x86(__inout __deref __drv_acquiresExclusiveResource(KeSpinLockType) PKSPIN_LOCK SpinLock,
            __out __deref __drv_when(LowestAssumedIrql < DISPATCH_LEVEL, __drv_savesIRQL) PKIRQL OldIrql,
            __in KIRQL LowestAssumedIrql)
    {
        if (LowestAssumedIrql >= DISPATCH_LEVEL)
        {
            ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

            KeAcquireSpinLockAtDpcLevel(SpinLock);
        }
        else
        {
            KeAcquireSpinLock(SpinLock, OldIrql);
        }
    }

    FORCEINLINE
        VOID
        __drv_requiresIRQL(DISPATCH_LEVEL)
        QCacheReleaseLock_x86(
            __inout __deref __drv_releasesExclusiveResource(KeSpinLockType) PKSPIN_LOCK SpinLock,
            __in __drv_when(*LowestAssumedIrql < DISPATCH_LEVEL, __drv_restoresIRQL) KIRQL OldIrql,
            __inout __deref PKIRQL LowestAssumedIrql)
    {
        ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

        if (*LowestAssumedIrql >= DISPATCH_LEVEL)
        {
            KeReleaseSpinLockFromDpcLevel(SpinLock);
        }
        else
        {
            KeReleaseSpinLock(SpinLock, OldIrql);
            *LowestAssumedIrql = OldIrql;
        }
    }

#ifdef _AMD64_

#define QCacheAcquireLock QCacheAcquireLock_x64

#define QCacheReleaseLock QCacheReleaseLock_x64

#else

#define QCacheAcquireLock(SpinLock, LockHandle, LowestAssumedIrql) \
    { \
        (LockHandle)->LockQueue.Lock = (SpinLock); \
        QCacheAcquireLock_x86((LockHandle)->LockQueue.Lock, &(LockHandle)->OldIrql, (LowestAssumedIrql)); \
    }

#define QCacheReleaseLock(LockHandle, LowestAssumedIrql) \
    { \
        QCacheReleaseLock_x86((LockHandle)->LockQueue.Lock, (LockHandle)->OldIrql, (LowestAssumedIrql)); \
    }

#endif

}