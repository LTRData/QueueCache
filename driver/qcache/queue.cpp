#include "qcache.h"

NTSTATUS
QCacheQueueLazyWriteIrp(IN PDEVICE_EXTENSION DeviceExtension,
    IN PIRP Irp);

//
// Adds an IRP to the request queue. If the request is an IRP_MJ_WRITE or
// IRP_MJ_FLUSH_BUFFERS and pool memory conditions are good, the IRP may
// be immediately completed and the actual operation carried out in background.
//

NTSTATUS
QCacheQueueIrp(
    PDEVICE_EXTENSION DeviceExtension,
    PIRP Irp)
{
    auto io_stack = IoGetCurrentIrpStackLocation(Irp);

    if (DeviceExtension->Statistics.IsCached &&
        (io_stack->MajorFunction == IRP_MJ_WRITE ||
            io_stack->MajorFunction == IRP_MJ_FLUSH_BUFFERS))
    {
        if ((QCacheKernelHighNonPagedPoolCondition == NULL ||
            KeReadStateEvent(QCacheKernelHighNonPagedPoolCondition)) &&
            (QCacheKernelHighMemoryCondition == NULL ||
                KeReadStateEvent(QCacheKernelHighMemoryCondition)) &&
            DeviceExtension->Statistics.WriteQueueSize <
            DeviceExtension->Statistics.MaxQueueSize &&
            DeviceExtension->Statistics.WriteQueueItems <
            DeviceExtension->Statistics.MaxQueueItems)
        {
            KeResetEvent(QCacheLowMemCondition);

            return QCacheQueueLazyWriteIrp(DeviceExtension, Irp);
        }
        else
        {
            InterlockedIncrement64(&DeviceExtension->Statistics.LowMemQueued);

            KeSetEvent(QCacheLowMemCondition, 0, FALSE);
        }
    }

    Irp->IoStatus.Information = 0;

    auto item = new WRITE_QUEUE_ITEM;

    if (item == NULL)
    {
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        KdBreakPoint();

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    item->RemoveLock = &DeviceExtension->RemoveLock;

    auto status = IoAcquireRemoveLock(item->RemoveLock, item);

    if (!NT_SUCCESS(status))
    {
        DbgPrint(
            "QCacheQueueIrp:IoAcquireRemoveLock failed: DeviceExtension %p Item %p Status: 0x%X.\n",
            DeviceExtension, item, status);

        Irp->IoStatus.Status = status;

        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        KdBreakPoint();

        delete item;

        return status;
    }

    item->MajorFunction = io_stack->MajorFunction;

    item->Irp = Irp;

    IoMarkIrpPending(Irp);

    KLOCK_QUEUE_HANDLE lock_handle;
    KIRQL lowest_irql = PASSIVE_LEVEL;

    QCacheAcquireLock(&DeviceExtension->WriteQueueLock, &lock_handle,
        lowest_irql);

    InsertTailList(&DeviceExtension->WriteQueue, &item->ListEntry);

    DeviceExtension->Statistics.WriteQueueSize += sizeof(*item);

    ++DeviceExtension->Statistics.WriteQueueItems;

    if (DeviceExtension->Statistics.WriteQueueSize >
        DeviceExtension->Statistics.WriteQueueSizeTop)
    {
        DeviceExtension->Statistics.WriteQueueSizeTop =
            DeviceExtension->Statistics.WriteQueueSize;
    }

    if (DeviceExtension->Statistics.WriteQueueItems >
        DeviceExtension->Statistics.WriteQueueItemsTop)
    {
        DeviceExtension->Statistics.WriteQueueItemsTop =
            DeviceExtension->Statistics.WriteQueueItems;
    }

    QCacheReleaseLock(&lock_handle, &lowest_irql);

    KeSetEvent(&DeviceExtension->WriteQueueEvent, 0, FALSE);

    return STATUS_PENDING;
}


//
// Adds an IRP_MJ_WRITE or IRP_MJ_FLUSH_BUFFERS request to the queue and
// immediately completes the original request. The operation will
// be lazy-completed in background.
//

NTSTATUS
QCacheQueueLazyWriteIrp(
    IN PDEVICE_EXTENSION DeviceExtension,
    IN PIRP Irp)
{
    Irp->IoStatus.Information = 0;

    auto io_stack = IoGetCurrentIrpStackLocation(Irp);

    auto item = new WRITE_QUEUE_ITEM;

    if (item == NULL)
    {
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        KdBreakPoint();

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    item->RemoveLock = &DeviceExtension->RemoveLock;

    auto status = IoAcquireRemoveLock(item->RemoveLock, item);

    if (!NT_SUCCESS(status))
    {
        DbgPrint(
            "QCacheQueueLazyWriteIrp:IoAcquireRemoveLock failed: DeviceExtension %p Item %p Status: 0x%X.\n",
            DeviceExtension, item, status);

        Irp->IoStatus.Status = status;

        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        KdBreakPoint();

        delete item;

        return status;
    }

    item->MajorFunction = io_stack->MajorFunction;

    if (io_stack->MajorFunction == IRP_MJ_WRITE)
    {
        item->Offset = io_stack->Parameters.Write.ByteOffset;
        item->Length = io_stack->Parameters.Write.Length;

        auto system_buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress,
            NormalPagePriority);

        if (system_buffer == NULL)
        {
            Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);

            KdBreakPoint();

            return STATUS_INSUFFICIENT_RESOURCES;
        }

        item->Buffer = new UCHAR[io_stack->Parameters.Write.Length];

        if (item->Buffer == NULL)
        {
            delete item;

            Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);

            KdBreakPoint();

            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(item->Buffer, system_buffer,
            io_stack->Parameters.Write.Length);
    }

    KLOCK_QUEUE_HANDLE lock_handle;
    KIRQL lowest_irql = PASSIVE_LEVEL;

    QCacheAcquireLock(&DeviceExtension->WriteQueueLock, &lock_handle,
        lowest_irql);

    InsertTailList(&DeviceExtension->WriteQueue, &item->ListEntry);

    DeviceExtension->Statistics.WriteQueueSize +=
        sizeof(*item) + item->Length;

    ++DeviceExtension->Statistics.WriteQueueItems;

    if (DeviceExtension->Statistics.WriteQueueSize >
        DeviceExtension->Statistics.WriteQueueSizeTop)
    {
        DeviceExtension->Statistics.WriteQueueSizeTop =
            DeviceExtension->Statistics.WriteQueueSize;
    }

    if (DeviceExtension->Statistics.WriteQueueItems >
        DeviceExtension->Statistics.WriteQueueItemsTop)
    {
        DeviceExtension->Statistics.WriteQueueItemsTop =
            DeviceExtension->Statistics.WriteQueueItems;
    }

    QCacheReleaseLock(&lock_handle, &lowest_irql);

    KeSetEvent(&DeviceExtension->WriteQueueEvent, 0, FALSE);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    if (io_stack->MajorFunction == IRP_MJ_WRITE)
    {
        Irp->IoStatus.Information = io_stack->Parameters.Write.Length;
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}


VOID
QCacheDispatchQueuedItem(
    PDEVICE_EXTENSION DeviceExtension,
    PWRITE_QUEUE_ITEM Item)
{
    KEVENT event;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    PIRP lower_irp = NULL;

    if (Item->Irp != NULL)
    {
        // Requests like IRP_MJ_SHUTDOWN, IRP_MJ_DEVICE_CONTROL and similar
        // are never lazy-written, so we have an original IRP to complete
        // in those cases.

        auto io_stack = IoGetCurrentIrpStackLocation(Item->Irp);

        if ((io_stack->MajorFunction == IRP_MJ_DEVICE_CONTROL ||
            io_stack->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL ||
            io_stack->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL) &&
            (io_stack->Parameters.DeviceIoControl.IoControlCode ==
                IOCTL_QCACHE_OFF ||
                io_stack->Parameters.DeviceIoControl.IoControlCode ==
                IOCTL_QCACHE_FLUSH))
        {
            Item->Irp->IoStatus.Information = 0;
            Item->Irp->IoStatus.Status = STATUS_SUCCESS;
        }
        else
        {
            IoCopyCurrentIrpStackLocationToNext(Item->Irp);

            IoSetCompletionRoutine(Item->Irp, QCacheSynchronousIrpCompletion,
                &event, TRUE, TRUE, TRUE);

            lower_irp = Item->Irp;
        }
    }
    else
    {
        // Only supports lazy-writing IRP_MJ_WRITE and IRP_MJ_FLUSH_BUFFERS
        // requests, so we can simply use IoBuildSynchronousFsdRequest to
        // build one.

        IO_STATUS_BLOCK io_status;

        lower_irp = IoBuildSynchronousFsdRequest(Item->MajorFunction,
            DeviceExtension->TargetDeviceObject,
            Item->Buffer,
            Item->Length,
            &Item->Offset,
            &event,
            &io_status);

        if (lower_irp == NULL)
        {
            DeviceExtension->Statistics.LastErrorCode =
                STATUS_INSUFFICIENT_RESOURCES;

            KdBreakPoint();

            return;
        }

        auto lower_io_stack = IoGetNextIrpStackLocation(lower_irp);

        if (Item->MajorFunction == IRP_MJ_WRITE)
        {
            lower_irp->Flags |= IRP_WRITE_OPERATION | IRP_NOCACHE;
            lower_io_stack->Flags |= SL_WRITE_THROUGH;
        }
    }

    if (lower_irp != NULL)
    {
        auto status = IoCallDriver(DeviceExtension->TargetDeviceObject,
            lower_irp);

        if (status == STATUS_PENDING)
        {
            KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        }
    }

    if (Item->Irp != NULL)
    {
        IoCompleteRequest(Item->Irp,
            NT_SUCCESS(Item->Irp->IoStatus.Status) ?
            IO_DISK_INCREMENT : IO_NO_INCREMENT);

        Item->Irp = NULL;
    }

    KLOCK_QUEUE_HANDLE lock_handle;
    KIRQL lowest_irql = PASSIVE_LEVEL;

    QCacheAcquireLock(&DeviceExtension->WriteQueueLock, &lock_handle,
        lowest_irql);

    DeviceExtension->Statistics.WriteQueueSize -=
        sizeof(*Item) + Item->Length;

    --DeviceExtension->Statistics.WriteQueueItems;

    RemoveEntryList(&Item->ListEntry);

    QCacheReleaseLock(&lock_handle, &lowest_irql);

    IoReleaseRemoveLock(Item->RemoveLock, Item);

    delete Item;
}
