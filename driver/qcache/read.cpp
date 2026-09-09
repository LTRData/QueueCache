#include "qcache.h"

//
// Handles IRP_MJ_READ by first walking the pending lazy-write
// queue and only then if any ranges left to read, request those
// reads from underlying driver.
//

NTSTATUS
QCacheRead(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    auto device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (device_extension->Statistics.Size.QuadPart == 0)
    {
        return QCacheSendToNextDriver(DeviceObject, Irp);
    }

    Irp->IoStatus.Information = 0;

    auto io_stack = IoGetCurrentIrpStackLocation(Irp);

    NTSTATUS status;

    InterlockedIncrement64(&device_extension->Statistics.ReadRequests);

    InterlockedAdd64(&device_extension->Statistics.ReadBytes,
        io_stack->Parameters.Read.Length);

    if (io_stack->Parameters.Read.Length == 0)
    {
        return QCacheIgnore(DeviceObject, Irp);
    }

    if (io_stack->Parameters.Read.Length >
        device_extension->Statistics.LargestReadSize)
    {
        device_extension->Statistics.LargestReadSize =
            io_stack->Parameters.Read.Length;
        KdPrint(("QCache: Largest read size is now %u KB\n",
            device_extension->Statistics.LargestReadSize >> 10));
    }

    if (((io_stack->Parameters.Read.ByteOffset.QuadPart & 0x1ff) != 0) ||
        ((io_stack->Parameters.Read.Length & 0x1ff) != 0))
    {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        KdBreakPoint();

        return STATUS_INVALID_PARAMETER;
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

    auto system_buffer = (PUCHAR)
        MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);

    if (system_buffer == NULL)
    {
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        KdBreakPoint();

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RTL_BITMAP bitmap;

    WPoolMem<ULONG, NonPagedPoolNx> bitmap_buffer(
        (io_stack->Parameters.Read.Length >> 9) + sizeof(ULONG) - 1);

    if (!bitmap_buffer)
    {
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        KdBreakPoint();

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    bitmap_buffer.Clear();

    RtlInitializeBitMap(&bitmap, bitmap_buffer,
        io_stack->Parameters.Read.Length >> 9);

    KLOCK_QUEUE_HANDLE lock_handle;

    KIRQL lowest_irql = PASSIVE_LEVEL;

    bool any_from_write_queue = false;

    QCacheAcquireLock(&device_extension->WriteQueueLock, &lock_handle,
        lowest_irql);

    for (
        auto entry = device_extension->WriteQueue.Flink;
        entry != &device_extension->WriteQueue;
        entry = entry->Flink)
    {
        auto item = CONTAINING_RECORD(entry, WRITE_QUEUE_ITEM, ListEntry);

        if ((item->MajorFunction == IRP_MJ_WRITE) &&
            (item->Offset.QuadPart <
            (io_stack->Parameters.Read.ByteOffset.QuadPart +
            io_stack->Parameters.Read.Length)) &&
            ((item->Offset.QuadPart + item->Length) >
            io_stack->Parameters.Read.ByteOffset.QuadPart))
        {
            LONGLONG start_pos = max(item->Offset.QuadPart,
                io_stack->Parameters.Read.ByteOffset.QuadPart);

            LONGLONG end_pos = min(item->Offset.QuadPart + item->Length,
                io_stack->Parameters.Read.ByteOffset.QuadPart +
                io_stack->Parameters.Read.Length);

            RtlCopyMemory(system_buffer + start_pos -
                io_stack->Parameters.Read.ByteOffset.QuadPart,
                item->Buffer + start_pos - item->Offset.QuadPart,
                (SIZE_T)(end_pos - start_pos));

            RtlSetBits(&bitmap,
                (ULONG)((start_pos -
                io_stack->Parameters.Read.ByteOffset.QuadPart) >> 9),
                (ULONG)((end_pos - start_pos) >> 9));

            any_from_write_queue = true;

            ++device_extension->Statistics.ReadRequestsFromCache;
            device_extension->Statistics.ReadBytesFromCache +=
                end_pos - start_pos;
        }
    }

    QCacheReleaseLock(&lock_handle, &lowest_irql);

    // If none of buffer filled by write queue
    if (!any_from_write_queue)
    {
        InterlockedIncrement64(
            &device_extension->Statistics.ReadRequestsReroutedToOriginal);

        InterlockedAdd64(
            &device_extension->Statistics.ReadBytesReroutedToOriginal,
            io_stack->Parameters.Read.Length);

        IoSkipCurrentIrpStackLocation(Irp);

        return IoCallDriver(device_extension->TargetDeviceObject, Irp);
    }

    ULONG clear_index;
    auto clear_bits = RtlFindFirstRunClear(&bitmap, &clear_index);

    // If entire buffer filled by write queue
    if (clear_bits == 0)
    {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = io_stack->Parameters.Read.Length;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        KdPrint(("QCache:QCacheRead: Entire buffer filled by write queue\n"));

        return STATUS_SUCCESS;
    }

    PSCATTERED_IRP scatter;

    status = SCATTERED_IRP::Create(
        &scatter,
        DeviceObject,
        Irp,
        &device_extension->RemoveLock,
        (DeviceObject->Flags & DO_BUFFERED_IO) ?
        (PUCHAR)Irp->AssociatedIrp.SystemBuffer :
        NULL);

    if (!NT_SUCCESS(status))
    {
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        KdBreakPoint();

        return status;
    }

    ULONG splits = 0;

    do
    {
        LARGE_INTEGER lower_offset;
        lower_offset.QuadPart =
            io_stack->Parameters.Read.ByteOffset.QuadPart +
            ((LONGLONG)clear_index << 9);

        auto lower_irp = scatter->BuildIrp(
            IRP_MJ_READ,
            device_extension->TargetDeviceObject,
            clear_index << 9,
            clear_bits << 9,
            &lower_offset);

        if (lower_irp == NULL)
        {
            break;
        }

        InterlockedAdd64(&device_extension->Statistics.ReadBytesFromOriginal,
            (LONGLONG)clear_bits << 9);

        IoCallDriver(device_extension->TargetDeviceObject, lower_irp);

        clear_bits = RtlFindNextForwardRunClear(
            &bitmap, clear_index + clear_bits, &clear_index);

        ++splits;

    } while (clear_bits > 0);

    if (splits > 1)
    {
        InterlockedAdd64(&device_extension->Statistics.SplitReads, splits);
    }

    // Decrement reference counter and complete if all partials are finished
    scatter->Complete();

    return STATUS_PENDING;
}				// end QCacheReadWrite()


