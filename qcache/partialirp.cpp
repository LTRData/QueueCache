#include "qcache.h"

PIRP SCATTERED_IRP::BuildIrp(
    UCHAR MajorFunction,
    PDEVICE_OBJECT DeviceObject,
    ULONG OriginalIrpOffset,
    ULONG BytesThisIrp,
    PLARGE_INTEGER LowerDeviceOffset)
{
    auto io_stack = IoGetCurrentIrpStackLocation(OriginalIrp);

    PIRP lower_irp = NULL;
    PIO_STACK_LOCATION lower_io_stack = NULL;

    if ((DeviceObject->Flags & DO_DIRECT_IO) &&
        (OriginalDeviceObject->Flags & DO_BUFFERED_IO))
    {
        lower_irp = IoBuildAsynchronousFsdRequest(
            MajorFunction,
            DeviceObject,
            SystemBuffer + OriginalIrpOffset,
            BytesThisIrp,
            LowerDeviceOffset,
            NULL);

        if (lower_irp != NULL)
        {
            lower_io_stack = IoGetNextIrpStackLocation(lower_irp);
        }
    }
    else
    {
        lower_irp =
            IoAllocateIrp(DeviceObject->StackSize,
            FALSE);

        if (lower_irp != NULL)
        {
            lower_io_stack = IoGetNextIrpStackLocation(lower_irp);
            lower_io_stack->MajorFunction = MajorFunction;
            lower_io_stack->Parameters.Write.ByteOffset =
                *LowerDeviceOffset;
            lower_io_stack->Parameters.Write.Length = BytesThisIrp;

            if (DeviceObject->Flags & DO_DIRECT_IO)
            {
                auto mdl = IoAllocateMdl(
                    (PUCHAR)
                    MmGetMdlVirtualAddress(OriginalIrp->MdlAddress) +
                    OriginalIrpOffset,
                    BytesThisIrp, FALSE, FALSE, lower_irp);

                if (mdl == NULL)
                {
                    IoFreeIrp(lower_irp);
                    InterlockedExchange(&LastFailedStatus,
                        STATUS_INSUFFICIENT_RESOURCES);
                    return NULL;
                }

                IoBuildPartialMdl(
                    OriginalIrp->MdlAddress,
                    mdl, (PUCHAR)
                    MmGetMdlVirtualAddress(OriginalIrp->MdlAddress) +
                    OriginalIrpOffset,
                    BytesThisIrp);
            }
            else
            {
                if (SystemBuffer == NULL)
                {
                    SystemBuffer =
                        (PUCHAR)MmGetSystemAddressForMdlSafe(
                        OriginalIrp->MdlAddress, HighPagePriority);

                    if (SystemBuffer == NULL)
                    {
                        IoFreeIrp(lower_irp);
                        InterlockedExchange(&LastFailedStatus,
                            STATUS_INSUFFICIENT_RESOURCES);
                        return NULL;
                    }

                    AllocatedBuffer =
                        new UCHAR[io_stack->Parameters.Write.Length];

                    if (AllocatedBuffer == NULL)
                    {
                        IoFreeIrp(lower_irp);
                        InterlockedExchange(&LastFailedStatus,
                            STATUS_INSUFFICIENT_RESOURCES);
                        return NULL;
                    }

                    if (MajorFunction == IRP_MJ_WRITE)
                    {
                        RtlCopyMemory(AllocatedBuffer,
                            SystemBuffer,
                            io_stack->Parameters.Write.Length);
                    }
                    else if (MajorFunction == IRP_MJ_READ)
                    {
                        CopyBack = TRUE;
                    }
                }

                auto buffer = AllocatedBuffer != NULL ?
                AllocatedBuffer : SystemBuffer;

                if (DeviceObject->Flags & DO_BUFFERED_IO)
                {
                    lower_irp->AssociatedIrp.SystemBuffer =
                        buffer + OriginalIrpOffset;
                }
                else
                {
                    lower_irp->UserBuffer = buffer + OriginalIrpOffset;
                }
            }
        }
    }

    if (lower_irp == NULL)
    {
        InterlockedExchange(&LastFailedStatus,
            STATUS_INSUFFICIENT_RESOURCES);

        return NULL;
    }

    lower_irp->Tail.Overlay.Thread = OriginalIrp->Tail.Overlay.Thread;

    if (MajorFunction == IRP_MJ_WRITE)
    {
        lower_irp->Flags |= IRP_WRITE_OPERATION | SL_WRITE_THROUGH |
            IRP_NOCACHE;
    }
    else if (MajorFunction == IRP_MJ_READ)
    {
        lower_irp->Flags |= IRP_READ_OPERATION | IRP_NOCACHE;
    }

    IoSetCompletionRoutine(lower_irp, IrpCompletionRoutine,
        this, TRUE, TRUE, TRUE);

    InterlockedIncrement(&ScatterCount);

    return lower_irp;
}

NTSTATUS
SCATTERED_IRP::IrpCompletionRoutine(
PDEVICE_OBJECT DeviceObject,
PIRP Irp,
PVOID Context
)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    __analysis_assume(Context != NULL);

    auto scatter = (PSCATTERED_IRP)Context;

    if (NT_SUCCESS(Irp->IoStatus.Status))
    {
        InterlockedAddPtr(&scatter->BytesCompleted, Irp->IoStatus.Information);
    }
    else
    {
        KdPrint(("QCache: Lower level I/O failed: 0x%X\n",
            Irp->IoStatus.Status));

        KdBreakPoint();

        InterlockedExchange(&scatter->LastFailedStatus, Irp->IoStatus.Status);
    }

    if (Irp->MdlAddress != scatter->OriginalIrp->MdlAddress)
    {
        QCacheFreeIrpWithMdls(Irp);
    }
    else
    {
        IoFreeIrp(Irp);
    }

    scatter->Complete();

    return STATUS_MORE_PROCESSING_REQUIRED;
}

