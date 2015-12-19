#include "qcache.h"

NTSTATUS
QCacheDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    auto device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (device_extension->Statistics.Size.QuadPart == 0)
    {
        return QCacheSendToNextDriver(DeviceObject, Irp);
    }

    auto io_stack = IoGetCurrentIrpStackLocation(Irp);
    
    NTSTATUS status;

    Irp->IoStatus.Information = 0;

    switch (io_stack->Parameters.DeviceIoControl.IoControlCode)
    {
    case IOCTL_QCACHE_GET_DEVICE_DATA:
    {
        if (io_stack->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(DEVICE_STATISTICS))
        {
            status = STATUS_BUFFER_TOO_SMALL;

            Irp->IoStatus.Status = status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }

        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
            &device_extension->Statistics,
            sizeof(DEVICE_STATISTICS));

        status = STATUS_SUCCESS;

        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = sizeof(DEVICE_STATISTICS);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    default:
        break;
    }

    IoSkipCurrentIrpStackLocation(Irp);

    //
    //
    // Return the results of the call to the disk driver.
    //

    return IoCallDriver(device_extension->TargetDeviceObject, Irp);

}				// end QCacheDeviceControl()


