#include "qcache.h"

NTSTATUS
QCacheWrite(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    auto device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (device_extension->Statistics.Size.QuadPart == 0)
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

