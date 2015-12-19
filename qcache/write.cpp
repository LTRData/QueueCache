#include "qcache.h"

NTSTATUS
QCacheWrite(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    auto device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (!device_extension->Statistics.IsCached)
    {
        return QCacheSendToNextDriver(DeviceObject, Irp);
    }

    auto io_stack = IoGetCurrentIrpStackLocation(Irp);

    InterlockedIncrement64(&device_extension->Statistics.WriteRequests);

    InterlockedAdd64(&device_extension->Statistics.WrittenBytes,
        io_stack->Parameters.Write.Length);

    if (io_stack->Parameters.Write.Length == 0)
    {
        return QCacheIgnore(DeviceObject, Irp);
    }

    LONGLONG highest_byte =
        io_stack->Parameters.Read.ByteOffset.QuadPart +
        io_stack->Parameters.Read.Length;

    if ((io_stack->Parameters.Read.ByteOffset.QuadPart >=
        device_extension->Statistics.Size.QuadPart) ||
        (highest_byte <= 0) ||
        (highest_byte > device_extension->Statistics.Size.QuadPart))
    {
        Irp->IoStatus.Status = STATUS_END_OF_MEDIA;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        KdBreakPoint();

        return STATUS_END_OF_MEDIA;
    }

    if (io_stack->Parameters.Write.Length >
        device_extension->Statistics.LargestWriteSize)
    {
        device_extension->Statistics.LargestWriteSize =
            io_stack->Parameters.Write.Length;

        KdPrint(("QCache: Largest write size is now %u KB\n",
            device_extension->Statistics.LargestWriteSize >> 10));
    }

    return QCacheQueueIrp(device_extension, Irp);

}				// end QCacheReadWrite()


NTSTATUS
QCacheFlushBuffers(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    auto device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    return QCacheQueueIrp(device_extension, Irp);
}


NTSTATUS
QCacheShutdown(PDEVICE_OBJECT DeviceObject,
PIRP Irp)
{
    auto device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    device_extension->Statistics.IsCached = FALSE;

    KdBreakPoint();

    return QCacheQueueIrp(device_extension, Irp);
}


NTSTATUS
QCacheQueueIrp(
PDEVICE_EXTENSION DeviceExtension,
PIRP Irp)
{
    if (DeviceExtension->Statistics.IsCached)
    {
        if ((QCacheKernelHighNonPagedPoolCondition == NULL ||
            KeReadStateEvent(QCacheKernelHighNonPagedPoolCondition)) &&
            (QCacheKernelHighMemoryCondition == NULL ||
            KeReadStateEvent(QCacheKernelHighMemoryCondition)))
        {
            KeResetEvent(QCacheLowMemCondition);

            return QCacheDeferIrp(DeviceExtension, Irp);
        }
        else
        {
            InterlockedIncrement64(&DeviceExtension->Statistics.LowMemQueued);
            KeSetEvent(QCacheLowMemCondition, 0, FALSE);
        }
    }

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


NTSTATUS
QCacheDeferIrp(
PDEVICE_EXTENSION DeviceExtension,
PIRP Irp)
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
            "QCacheDeferIrp:IoAcquireRemoveLock failed: DeviceExtension %p Item %p Status: 0x%X.\n",
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
QCacheDeferredIrp(
PDEVICE_EXTENSION DeviceExtension,
PWRITE_QUEUE_ITEM Item)
{
    KEVENT event;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    IO_STATUS_BLOCK io_status;

    PIRP lower_irp;
    
    if (Item->Irp != NULL)
    {
        lower_irp = Item->Irp;
    }
    else
    {
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

    auto status = IoCallDriver(DeviceExtension->TargetDeviceObject,
        lower_irp);

    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
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
