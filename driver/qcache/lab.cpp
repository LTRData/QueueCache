// SPDX-License-Identifier: MIT
// Guarded secondary-disk lab filter. The legacy cache engine is never linked here.
// Default is pass-through; QCACHE_WRITE_LAB explicitly adds the new bounded cache.
#include <ntifs.h>
#include <ntdddisk.h>
#include "qcstats.h"
#if QCACHE_WRITE_LAB
#include "writecache.h"
#endif

struct LAB_EXTENSION {
    PDEVICE_OBJECT Lower;
    IO_REMOVE_LOCK RemoveLock;
    volatile LONG64 ReadBytes;
    volatile LONG64 WrittenBytes;
    LARGE_INTEGER Size;
#if QCACHE_WRITE_LAB
    QC_CACHE Cache;
#endif
#if QCACHE_SERIALIZED_LAB
    IO_CSQ Csq;
    KSPIN_LOCK QueueLock;
    LIST_ENTRY Pending;
    KEVENT WorkAvailable;
    HANDLE Worker;
    BOOLEAN Closing; // Protected by QueueLock, including the CSQ insertion gate.
#endif
};
static WCHAR ExpectedDriverKey[512];
static UNICODE_STRING AllowedDriverKey;
extern "C" DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE LabAddDevice;
DRIVER_DISPATCH LabDispatch;
DRIVER_UNLOAD LabUnload;
IO_COMPLETION_ROUTINE LabCompletion;
IO_COMPLETION_ROUTINE LabStartCompletion;

static NTSTATUS Complete(PIRP irp, NTSTATUS status, ULONG_PTR bytes = 0) {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = bytes;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS LabCompletion(PDEVICE_OBJECT, PIRP irp, PVOID context) {
    auto ext = static_cast<LAB_EXTENSION*>(context);
    if (irp->PendingReturned) IoMarkIrpPending(irp);
    IoReleaseRemoveLock(&ext->RemoveLock, irp);
    return STATUS_CONTINUE_COMPLETION;
}

NTSTATUS LabStartCompletion(PDEVICE_OBJECT, PIRP, PVOID context) {
    KeSetEvent(static_cast<PKEVENT>(context), IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS Forward(LAB_EXTENSION* ext, PIRP irp) {
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, LabCompletion, ext, TRUE, TRUE, TRUE);
    return IoCallDriver(ext->Lower, irp);
}

#if QCACHE_SERIALIZED_LAB
// Worker foundation: original requests only, NO early acknowledgements or cached data.
// CSQ owns cancellation while queued; the lower driver owns it after dequeue/forward.
static LAB_EXTENSION* QueueOwner(PIO_CSQ csq) { return CONTAINING_RECORD(csq, LAB_EXTENSION, Csq); }
static NTSTATUS QueueInsert(PIO_CSQ csq, PIRP irp, PVOID) {
    auto ext = QueueOwner(csq);
    if (ext->Closing) return STATUS_DELETE_PENDING;
    InsertTailList(&ext->Pending, &irp->Tail.Overlay.ListEntry);
    KeSetEvent(&ext->WorkAvailable, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}
static void QueueRemove(PIO_CSQ, PIRP irp) { RemoveEntryList(&irp->Tail.Overlay.ListEntry); }
static PIRP QueuePeek(PIO_CSQ csq, PIRP irp, PVOID) {
    auto head = &QueueOwner(csq)->Pending;
    auto next = irp ? irp->Tail.Overlay.ListEntry.Flink : head->Flink;
    return next == head ? nullptr : CONTAINING_RECORD(next, IRP, Tail.Overlay.ListEntry);
}
static void QueueAcquire(PIO_CSQ csq, PKIRQL irql) { KeAcquireSpinLock(&QueueOwner(csq)->QueueLock, irql); }
static void QueueRelease(PIO_CSQ csq, KIRQL irql) { KeReleaseSpinLock(&QueueOwner(csq)->QueueLock, irql); }
static void QueueCancel(PIO_CSQ csq, PIRP irp) {
    IoReleaseRemoveLock(&QueueOwner(csq)->RemoveLock, irp);
    Complete(irp, STATUS_CANCELLED);
}
static void RequestWorker(PVOID context) {
    auto ext = static_cast<LAB_EXTENSION*>(context);
    for (;;) {
        auto irp = IoCsqRemoveNextIrp(&ext->Csq, nullptr);
        if (irp) {
#if QCACHE_WRITE_LAB
            auto status = QcCacheProcess(&ext->Cache, irp, InterlockedCompareExchange64(&ext->Size.QuadPart, 0, 0));
            auto bytes = NT_SUCCESS(status) ? irp->IoStatus.Information : 0;
#else
            KEVENT completed;
            KeInitializeEvent(&completed, NotificationEvent, FALSE);
            IoCopyCurrentIrpStackLocationToNext(irp);
            IoSetCompletionRoutine(irp, LabStartCompletion, &completed, TRUE, TRUE, TRUE);
            auto status = IoCallDriver(ext->Lower, irp);
            if (status == STATUS_PENDING)
                KeWaitForSingleObject(&completed, Executive, KernelMode, FALSE, nullptr);
            // Completion retained this IRP. Preserve lower status and exact byte count.
            status = irp->IoStatus.Status;
            auto bytes = irp->IoStatus.Information;
#endif
            IoReleaseRemoveLock(&ext->RemoveLock, irp);
            Complete(irp, status, bytes);
            continue;
        }
        // Clear/check under insertion's lock: no lost wakeup if a request races idle.
        KIRQL irql;
        KeAcquireSpinLock(&ext->QueueLock, &irql);
        const bool empty = IsListEmpty(&ext->Pending) != FALSE;
        const bool stop = ext->Closing && empty;
        if (empty) KeClearEvent(&ext->WorkAvailable);
        KeReleaseSpinLock(&ext->QueueLock, irql);
        if (stop) break;
        if (empty) KeWaitForSingleObject(&ext->WorkAvailable, Executive, KernelMode, FALSE, nullptr);
    }
#if QCACHE_WRITE_LAB
    QcCacheBarrier(&ext->Cache, TRUE);
#endif
    PsTerminateSystemThread(STATUS_SUCCESS);
}
static NTSTATUS QueueRequest(LAB_EXTENSION* ext, PIRP irp) {
    IoMarkIrpPending(irp);
    auto status = IoCsqInsertIrpEx(&ext->Csq, irp, nullptr, nullptr);
    if (!NT_SUCCESS(status)) {
        IoReleaseRemoveLock(&ext->RemoveLock, irp);
        Complete(irp, status);
    }
    // Pending was marked even if insertion rejected or cancellation completed inline.
    return STATUS_PENDING;
}
#endif

NTSTATUS LabDispatch(PDEVICE_OBJECT device, PIRP irp) {
    auto ext = static_cast<LAB_EXTENSION*>(device->DeviceExtension);
    auto status = IoAcquireRemoveLock(&ext->RemoveLock, irp);
    if (!NT_SUCCESS(status)) return Complete(irp, status);
    auto stack = IoGetCurrentIrpStackLocation(irp);
    if (stack->MajorFunction == IRP_MJ_PNP) {
        if (stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
#if QCACHE_SERIALIZED_LAB
            KIRQL irql;
            KeAcquireSpinLock(&ext->QueueLock, &irql);
            ext->Closing = TRUE;
            KeSetEvent(&ext->WorkAvailable, IO_NO_INCREMENT, FALSE);
            KeReleaseSpinLock(&ext->QueueLock, irql);
#endif
            // Close admission and wait before allowing the lower stack to disappear.
            IoReleaseRemoveLockAndWait(&ext->RemoveLock, irp);
#if QCACHE_SERIALIZED_LAB
            ZwWaitForSingleObject(ext->Worker, FALSE, nullptr);
            ZwClose(ext->Worker);
#endif
#if QCACHE_WRITE_LAB
            IoUnregisterShutdownNotification(device);
            QcCacheDestroy(&ext->Cache);
#endif
            auto lower = ext->Lower;
            IoSkipCurrentIrpStackLocation(irp);
            status = IoCallDriver(lower, irp);
            IoDetachDevice(lower);
            IoDeleteDevice(device);
            return status;
        }
#if QCACHE_WRITE_LAB
        if (stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL) {
            InterlockedExchange(&ext->Cache.Gone, TRUE);
            KeSetEvent(&ext->Cache.Changed, IO_NO_INCREMENT, FALSE);
            KeSetEvent(&ext->Cache.Wake, IO_NO_INCREMENT, FALSE);
        }
        if (stack->MinorFunction == IRP_MN_QUERY_STOP_DEVICE || stack->MinorFunction == IRP_MN_QUERY_REMOVE_DEVICE)
            return QueueRequest(ext, irp);
#endif
        if (stack->MinorFunction == IRP_MN_DEVICE_USAGE_NOTIFICATION &&
            stack->Parameters.UsageNotification.InPath &&
            (stack->Parameters.UsageNotification.Type == DeviceUsageTypePaging ||
             stack->Parameters.UsageNotification.Type == DeviceUsageTypeHibernation ||
             stack->Parameters.UsageNotification.Type == DeviceUsageTypeDumpFile)) {
            IoReleaseRemoveLock(&ext->RemoveLock, irp);
            return Complete(irp, STATUS_NOT_SUPPORTED);
        }
        if (stack->MinorFunction == IRP_MN_START_DEVICE) {
            KEVENT event;
            KeInitializeEvent(&event, NotificationEvent, FALSE);
            IoCopyCurrentIrpStackLocationToNext(irp);
            IoSetCompletionRoutine(irp, LabStartCompletion, &event, TRUE, TRUE, TRUE);
            status = IoCallDriver(ext->Lower, irp);
            if (status == STATUS_PENDING) KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
            status = irp->IoStatus.Status;
            if (NT_SUCCESS(status)) {
                GET_LENGTH_INFORMATION length = {};
                IO_STATUS_BLOCK iosb = {};
                KeClearEvent(&event);
                auto query = IoBuildDeviceIoControlRequest(IOCTL_DISK_GET_LENGTH_INFO, ext->Lower,
                    nullptr, 0, &length, sizeof(length), FALSE, &event, &iosb);
                if (query) {
                    auto queryStatus = IoCallDriver(ext->Lower, query);
                    if (queryStatus == STATUS_PENDING) KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
                    if (NT_SUCCESS(iosb.Status) && iosb.Information >= sizeof(length))
                        InterlockedExchange64(&ext->Size.QuadPart, length.Length.QuadPart);
                }
#if QCACHE_WRITE_LAB
                DISK_GEOMETRY geometry = {};
                iosb = {}; KeClearEvent(&event);
                query = IoBuildDeviceIoControlRequest(IOCTL_DISK_GET_DRIVE_GEOMETRY, ext->Lower,
                    nullptr, 0, &geometry, sizeof(geometry), FALSE, &event, &iosb);
                if (query) {
                    auto queryStatus = IoCallDriver(ext->Lower, query);
                    if (queryStatus == STATUS_PENDING) KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
                    if (NT_SUCCESS(iosb.Status) && iosb.Information >= sizeof(geometry)) ext->Cache.SectorBytes = geometry.BytesPerSector;
                }
#endif
            }
            IoReleaseRemoveLock(&ext->RemoveLock, irp);
            return Complete(irp, status, irp->IoStatus.Information);
        }
    }
    if (stack->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
        auto code = stack->Parameters.DeviceIoControl.IoControlCode;
#if QCACHE_WRITE_LAB
        if (code == IOCTL_QCACHE_STATE_V2) {
            if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(QC_STATE_V2)) {
                IoReleaseRemoveLock(&ext->RemoveLock, irp); return Complete(irp, STATUS_BUFFER_TOO_SMALL);
            }
            QC_STATE_V2 state; QcCacheSnapshotV2(&ext->Cache, &state);
            state.Base.DeviceBytes = InterlockedCompareExchange64(&ext->Size.QuadPart, 0, 0);
            if (ext->Cache.Gone) state.Base.Flags |= 16;
            RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &state, sizeof(state));
            IoReleaseRemoveLock(&ext->RemoveLock, irp); return Complete(irp, STATUS_SUCCESS, sizeof(state));
        }
        if (code == IOCTL_QCACHE_DIAGNOSTICS_V1) {
            if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(QC_DIAGNOSTICS)) {
                IoReleaseRemoveLock(&ext->RemoveLock, irp); return Complete(irp, STATUS_BUFFER_TOO_SMALL);
            }
            QC_DIAGNOSTICS diagnostics; QcCacheDiagnostics(&ext->Cache, &diagnostics);
            RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &diagnostics, sizeof(diagnostics));
            IoReleaseRemoveLock(&ext->RemoveLock, irp); return Complete(irp, STATUS_SUCCESS, sizeof(diagnostics));
        }
        if (code == IOCTL_QCACHE_STATE_V1) {
            if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(QC_STATE)) {
                IoReleaseRemoveLock(&ext->RemoveLock, irp); return Complete(irp, STATUS_BUFFER_TOO_SMALL);
            }
            QC_STATE state; QcCacheSnapshot(&ext->Cache, &state);
            state.DeviceBytes = InterlockedCompareExchange64(&ext->Size.QuadPart, 0, 0);
            if (ext->Cache.Gone) state.Flags |= 16;
            RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &state, sizeof(state));
            IoReleaseRemoveLock(&ext->RemoveLock, irp); return Complete(irp, STATUS_SUCCESS, sizeof(state));
        }
#endif
        if (code == IOCTL_QCACHE_GET_DEVICE_DATA) {
            if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(DEVICE_STATISTICS)) {
                IoReleaseRemoveLock(&ext->RemoveLock, irp);
                return Complete(irp, STATUS_BUFFER_TOO_SMALL);
            }
            DEVICE_STATISTICS stats = {};
            stats.Version = sizeof(stats);
            stats.Size.QuadPart = InterlockedCompareExchange64(&ext->Size.QuadPart, 0, 0);
            stats.ReadBytes = InterlockedCompareExchange64(&ext->ReadBytes, 0, 0);
            stats.WrittenBytes = InterlockedCompareExchange64(&ext->WrittenBytes, 0, 0);
#if QCACHE_WRITE_LAB
            QC_STATE state; QcCacheSnapshot(&ext->Cache, &state);
            stats.IsCached = (state.Flags & 1) != 0; stats.LastErrorCode = state.LastError;
            stats.WriteQueueItems = state.OccupiedSlots; stats.WriteQueueSize = state.DirtyBytes;
            stats.WriteQueueSizeTop = state.PeakDirtyBytes; stats.MaxQueueSize = state.PayloadCapacity;
            stats.LowMemQueued = state.ThrottleWaits;
#endif
            RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &stats, sizeof(stats));
            IoReleaseRemoveLock(&ext->RemoveLock, irp);
            return Complete(irp, STATUS_SUCCESS, sizeof(stats));
        }
        if (code == IOCTL_QCACHE_ON || code == IOCTL_QCACHE_OFF || code == IOCTL_QCACHE_FLUSH) {
            IoReleaseRemoveLock(&ext->RemoveLock, irp);
            return Complete(irp, STATUS_NOT_SUPPORTED);
        }
    }
    if (stack->MajorFunction == IRP_MJ_READ) InterlockedAdd64(&ext->ReadBytes, stack->Parameters.Read.Length);
    if (stack->MajorFunction == IRP_MJ_WRITE) InterlockedAdd64(&ext->WrittenBytes, stack->Parameters.Write.Length);
#if QCACHE_SERIALIZED_LAB
#if QCACHE_WRITE_LAB
    if (stack->MajorFunction == IRP_MJ_SHUTDOWN ||
        (stack->MajorFunction == IRP_MJ_POWER && stack->MinorFunction == IRP_MN_SET_POWER && stack->Parameters.Power.Type == DevicePowerState))
        return QueueRequest(ext, irp);
#endif
    if (stack->MajorFunction == IRP_MJ_READ || stack->MajorFunction == IRP_MJ_WRITE ||
        stack->MajorFunction == IRP_MJ_FLUSH_BUFFERS || stack->MajorFunction == IRP_MJ_DEVICE_CONTROL ||
        stack->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL)
        return QueueRequest(ext, irp);
#endif
    // Includes real flush/write-through, power, shutdown, TRIM and all unknown IOCTLs.
    // Never synthesize success for a storage operation.
    return Forward(ext, irp);
}

NTSTATUS LabAddDevice(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo) {
    WCHAR key[512] = {};
    ULONG required = 0;
    auto status = IoGetDeviceProperty(pdo, DevicePropertyDriverKeyName, sizeof(key), key, &required);
    if (!NT_SUCCESS(status) || required < sizeof(WCHAR) || required > sizeof(key) ||
        key[required / sizeof(WCHAR) - 1] != 0) return STATUS_SUCCESS;
    UNICODE_STRING actual;
    RtlInitUnicodeString(&actual, key);
    if (!RtlEqualUnicodeString(&actual, &AllowedDriverKey, TRUE)) return STATUS_SUCCESS;
    PDEVICE_OBJECT device = nullptr;
    status = IoCreateDevice(driver, sizeof(LAB_EXTENSION), nullptr, FILE_DEVICE_DISK,
        FILE_DEVICE_SECURE_OPEN, FALSE, &device);
    if (!NT_SUCCESS(status)) return status;
    auto ext = static_cast<LAB_EXTENSION*>(device->DeviceExtension);
    RtlZeroMemory(ext, sizeof(*ext));
    IoInitializeRemoveLock(&ext->RemoveLock, 'bLCQ', 0, 0);
    status = IoAttachDeviceToDeviceStackSafe(device, pdo, &ext->Lower);
    if (!NT_SUCCESS(status)) { IoDeleteDevice(device); return status; }
    device->Flags |= ext->Lower->Flags & (DO_DIRECT_IO | DO_BUFFERED_IO);
    // All harness code/data is nonpageable; do not advertise pageable power dispatch.
    device->Characteristics |= ext->Lower->Characteristics;
#if QCACHE_WRITE_LAB
    status = QcCacheInitialize(&ext->Cache, ext->Lower);
    if (NT_SUCCESS(status)) status = IoRegisterLastChanceShutdownNotification(device);
    if (!NT_SUCCESS(status)) { QcCacheDestroy(&ext->Cache); IoDetachDevice(ext->Lower); IoDeleteDevice(device); return status; }
#endif
#if QCACHE_SERIALIZED_LAB
    KeInitializeSpinLock(&ext->QueueLock);
    InitializeListHead(&ext->Pending);
    KeInitializeEvent(&ext->WorkAvailable, NotificationEvent, FALSE);
    status = IoCsqInitializeEx(&ext->Csq, QueueInsert, QueueRemove, QueuePeek,
        QueueAcquire, QueueRelease, QueueCancel);
    if (NT_SUCCESS(status)) {
        OBJECT_ATTRIBUTES attrs;
        InitializeObjectAttributes(&attrs, nullptr, OBJ_KERNEL_HANDLE, nullptr, nullptr);
        status = PsCreateSystemThread(&ext->Worker, THREAD_ALL_ACCESS, &attrs, nullptr, nullptr, RequestWorker, ext);
    }
    if (!NT_SUCCESS(status)) {
#if QCACHE_WRITE_LAB
        IoUnregisterShutdownNotification(device); QcCacheDestroy(&ext->Cache);
#endif
        IoDetachDevice(ext->Lower); IoDeleteDevice(device); return status;
    }
#endif
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

void LabUnload(PDRIVER_OBJECT driver) { NT_ASSERT(driver->DeviceObject == nullptr); UNREFERENCED_PARAMETER(driver); }

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath) {
    // No default/all-disk target. Installer records one exact device's driver key.
    OBJECT_ATTRIBUTES attrs;
    InitializeObjectAttributes(&attrs, registryPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    HANDLE key = nullptr;
    auto status = ZwOpenKey(&key, KEY_QUERY_VALUE, &attrs);
    if (!NT_SUCCESS(status)) return status;
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"LabAllowedDriverKey");
    alignas(KEY_VALUE_PARTIAL_INFORMATION) UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ExpectedDriverKey)] = {};
    ULONG required;
    status = ZwQueryValueKey(key, &name, KeyValuePartialInformation, buffer, sizeof(buffer), &required);
    ZwClose(key);
    if (!NT_SUCCESS(status)) return status;
    auto value = reinterpret_cast<KEY_VALUE_PARTIAL_INFORMATION*>(buffer);
    if (value->Type != REG_SZ || value->DataLength < 2 * sizeof(WCHAR) ||
        value->DataLength > sizeof(ExpectedDriverKey) || value->DataLength % sizeof(WCHAR) != 0)
        return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(ExpectedDriverKey, value->Data, value->DataLength);
    if (ExpectedDriverKey[value->DataLength / sizeof(WCHAR) - 1] != 0) return STATUS_INVALID_PARAMETER;
    RtlInitUnicodeString(&AllowedDriverKey, ExpectedDriverKey);
    if (AllowedDriverKey.Length + sizeof(WCHAR) != value->DataLength) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i) driver->MajorFunction[i] = LabDispatch;
    driver->DriverExtension->AddDevice = LabAddDevice;
    driver->DriverUnload = LabUnload;
    return STATUS_SUCCESS;
}
