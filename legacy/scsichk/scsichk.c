/* SPDX-License-Identifier: MS-LPL
 * See ../LICENSES/MS-LPL.txt and ../THIRD_PARTY_NOTICES.md.
 */

/*++
Copyright (C) Microsoft Corporation, 1991 - 1999

Module Name:

    scsichk.c

Abstract:

    This driver monitors disk accesses capturing performance data.

Environment:

    kernel mode only

Notes:

--*/


#define INITGUID

#include "ntddk.h"
#include "ntdddisk.h"
#include "stdarg.h"
#include "stdio.h"
#include <ntddvol.h>
#include <scsi.h>
#include <srb.h>
#include <srbhelper.h>

#include <mountdev.h>
#include "wmistr.h"
#include "wmidata.h"
#include "wmiguid.h"
#include "wmilib.h"

#include "ntstrsafe.h"

#include "scsichk.h"

#ifdef POOL_TAGGING
#ifdef ExAllocatePool
#undef ExAllocatePool
#endif
#define ExAllocatePool(a,b) ExAllocatePoolWithTag(a,b,'frPD')
#endif

#define DISKPERF_MAXSTR         64

//
// Device Extension
//

typedef struct _DEVICE_EXTENSION {

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
    // RemoveLock prevents removal of a device while it is busy.
    //

    IO_REMOVE_LOCK RemoveLock;

    //
    // Disk number for reference in WMI
    //

    ULONG       DiskNumber;

    //
    // If device is enabled for counting always
    //

    LONG        EnabledAlways;

    //
    // Use to keep track of Volume info from ntddvol.h
    //

    WCHAR StorageManagerName[8];

    //
    // Disk performance counters
    // and locals used to compute counters
    //

    ULONG   Processors;
    PDISK_PERFORMANCE DiskCounters;    // per processor counters
    LARGE_INTEGER LastIdleClock;
    LONG QueueDepth;
    LONG CountersEnabled;

    //
    // must synchronize paging path notifications
    //
    KEVENT PagingPathCountEvent;
    LONG  PagingPathCount;

    //
    // Physical Device name or WMI Instance Name
    //

    UNICODE_STRING PhysicalDeviceName;
    WCHAR PhysicalDeviceNameBuffer[DISKPERF_MAXSTR];

    //
    // Private context for using WmiLib
    //
    WMILIB_CONTEXT WmilibContext;

    KSPIN_LOCK LogLock;
    PLOGDATA LogData;

} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

#define DEVICE_EXTENSION_SIZE sizeof(DEVICE_EXTENSION)
#define PROCESSOR_COUNTERS_SIZE FIELD_OFFSET(DISK_PERFORMANCE, QueryTime)

/*
Layout of Per Processor Counters is a contiguous block of memory:
    Processor 1
+-----------------------+     +-----------------------+
|PROCESSOR_COUNTERS_SIZE| ... |PROCESSOR_COUNTERS_SIZE|
+-----------------------+     +-----------------------+
where PROCESSOR_COUNTERS_SIZE is less than sizeof(DISK_PERFORMANCE) since
we only put those we actually use for counting.
*/

UNICODE_STRING ScsiChkRegistryPath;


//
// Function declarations
//

DRIVER_INITIALIZE DriverEntry;

DRIVER_ADD_DEVICE ScsiChkAddDevice;

DRIVER_DISPATCH ScsiChkForwardIrpSynchronous;

_Dispatch_type_(IRP_MJ_PNP)
DRIVER_DISPATCH ScsiChkDispatchPnp;

_Dispatch_type_(IRP_MJ_POWER)
DRIVER_DISPATCH ScsiChkDispatchPower;

DRIVER_DISPATCH ScsiChkSendToNextDriver;

_Dispatch_type_(IRP_MJ_CREATE)
DRIVER_DISPATCH ScsiChkCreate;

_Dispatch_type_(IRP_MJ_READ)
_Dispatch_type_(IRP_MJ_WRITE)
DRIVER_DISPATCH ScsiChkReadWrite;

_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
DRIVER_DISPATCH ScsiChkDeviceControl;

_Dispatch_type_(IRP_MJ_SCSI)
DRIVER_DISPATCH ScsiChkScsi;

_Dispatch_type_(IRP_MJ_SYSTEM_CONTROL)
DRIVER_DISPATCH ScsiChkWmi;

_Dispatch_type_(IRP_MJ_FLUSH_BUFFERS)
_Dispatch_type_(IRP_MJ_SHUTDOWN)
DRIVER_DISPATCH ScsiChkShutdownFlush;

DRIVER_DISPATCH ScsiChkStartDevice;
DRIVER_DISPATCH ScsiChkRemoveDevice;

IO_COMPLETION_ROUTINE ScsiChkIoCompletion;
IO_COMPLETION_ROUTINE ScsiChkIrpCompletion;

DRIVER_UNLOAD ScsiChkUnload;


VOID
ScsiChkLogError(
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG UniqueId,
    IN NTSTATUS ErrorCode,
    IN NTSTATUS Status
    );

NTSTATUS
ScsiChkRegisterDevice(
    IN PDEVICE_OBJECT DeviceObject
    );

WMI_QUERY_REGINFO_CALLBACK DiskperfQueryWmiRegInfo;

WMI_QUERY_DATABLOCK_CALLBACK DiskperfQueryWmiDataBlock;

VOID
ScsiChkSyncFilterWithTarget(
    IN PDEVICE_OBJECT FilterDevice,
    IN PDEVICE_OBJECT TargetDevice
    );

WMI_FUNCTION_CONTROL_CALLBACK DiskperfWmiFunctionControl;

VOID
ScsiChkAddCounters(
    IN OUT PDISK_PERFORMANCE TotalCounters,
    IN PDISK_PERFORMANCE NewCounters,
    IN LARGE_INTEGER Frequency
    );

#if DBG

ULONG ScsiChkDebug = 0;

#endif

//
// Define the sections that allow for discarding (i.e. paging) some of
// the code.
//

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, ScsiChkCreate)
#pragma alloc_text (PAGE, ScsiChkAddDevice)
#pragma alloc_text (PAGE, ScsiChkDispatchPnp)
#pragma alloc_text (PAGE, ScsiChkStartDevice)
#pragma alloc_text (PAGE, ScsiChkRemoveDevice)
#pragma alloc_text (PAGE, ScsiChkUnload)
#pragma alloc_text (PAGE, ScsiChkWmi)
#pragma alloc_text (PAGE, DiskperfQueryWmiRegInfo)
#pragma alloc_text (PAGE, DiskperfQueryWmiDataBlock)
#pragma alloc_text (PAGE, ScsiChkRegisterDevice)
#pragma alloc_text (PAGE, ScsiChkSyncFilterWithTarget)
#endif

WMIGUIDREGINFO DiskperfGuidList[] =
{
    { &DiskPerfGuid,
      1,
      0
    }
};

#define DiskperfGuidCount (sizeof(DiskperfGuidList) / sizeof(WMIGUIDREGINFO))

#define USE_PERF_CTR

#ifdef USE_PERF_CTR
#define ScsiChkGetClock(a, b) (a) = KeQueryPerformanceCounter((b))
#else
#define ScsiChkGetClock(a, b) KeQuerySystemTime(&(a))
#endif


NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
    )

/*++

Routine Description:

    Installable driver initialization entry point.
    This entry point is called directly by the I/O manager to set up the disk
    performance driver. The driver object is set up and then the Pnp manager
    calls ScsiChkAddDevice to attach to the boot devices.

Arguments:

    DriverObject - The disk performance driver object.

    RegistryPath - pointer to a unicode string representing the path,
                   to driver-specific key in the registry.

Return Value:

    STATUS_SUCCESS if successful

--*/

{

    ULONG               ulIndex;
    PDRIVER_DISPATCH  * dispatch;

    //
    // Remember registry path
    //

    ScsiChkRegistryPath.MaximumLength = RegistryPath->Length
                                            + sizeof(UNICODE_NULL);
    ScsiChkRegistryPath.Buffer = ExAllocatePool(
                                    PagedPool,
                                    ScsiChkRegistryPath.MaximumLength);
    if (ScsiChkRegistryPath.Buffer != NULL)
    {
        RtlCopyUnicodeString(&ScsiChkRegistryPath, RegistryPath);
    } else {
        ScsiChkRegistryPath.Length = 0;
        ScsiChkRegistryPath.MaximumLength = 0;
    }

    //
    // Create dispatch points
    //
    for (ulIndex = 0, dispatch = DriverObject->MajorFunction;
         ulIndex <= IRP_MJ_MAXIMUM_FUNCTION;
         ulIndex++, dispatch++) {

        *dispatch = ScsiChkSendToNextDriver;
    }

    //
    // Set up the device driver entry points.
    //

    DriverObject->MajorFunction[IRP_MJ_CREATE]          = ScsiChkCreate;
    DriverObject->MajorFunction[IRP_MJ_READ]            = ScsiChkReadWrite;
    DriverObject->MajorFunction[IRP_MJ_WRITE]           = ScsiChkReadWrite;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]  = ScsiChkDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_SCSI]            = ScsiChkScsi;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL]  = ScsiChkWmi;

    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN]        = ScsiChkShutdownFlush;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]   = ScsiChkShutdownFlush;
    DriverObject->MajorFunction[IRP_MJ_PNP]             = ScsiChkDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]           = ScsiChkDispatchPower;

    DriverObject->DriverExtension->AddDevice            = ScsiChkAddDevice;
    DriverObject->DriverUnload                          = ScsiChkUnload;

    return(STATUS_SUCCESS);

} // end DriverEntry()

#define FILTER_DEVICE_PROPOGATE_FLAGS            0
#define FILTER_DEVICE_PROPOGATE_CHARACTERISTICS (FILE_REMOVABLE_MEDIA |  \
                                                 FILE_READ_ONLY_DEVICE | \
                                                 FILE_FLOPPY_DISKETTE    \
                                                 )

VOID
ScsiChkSyncFilterWithTarget(
    IN PDEVICE_OBJECT FilterDevice,
    IN PDEVICE_OBJECT TargetDevice
    )
{
    ULONG                   propFlags;

    PAGED_CODE();

    //
    // Propogate all useful flags from target to scsichk. MountMgr will look
    // at the scsichk object capabilities to figure out if the disk is
    // a removable and perhaps other things.
    //
    propFlags = TargetDevice->Flags & FILTER_DEVICE_PROPOGATE_FLAGS;
    FilterDevice->Flags |= propFlags;

    propFlags = TargetDevice->Characteristics & FILTER_DEVICE_PROPOGATE_CHARACTERISTICS;
    FilterDevice->Characteristics |= propFlags;
}

NTSTATUS
ScsiChkAddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject
    )
/*++
Routine Description:

    Creates and initializes a new filter device object FiDO for the
    corresponding PDO.  Then it attaches the device object to the device
    stack of the drivers for the device.

Arguments:

    DriverObject - Disk performance driver object.
    PhysicalDeviceObject - Physical Device Object from the underlying layered driver

Return Value:

    NTSTATUS
--*/

{
    NTSTATUS                status;
    PDEVICE_OBJECT          filterDeviceObject;
    PDEVICE_EXTENSION       deviceExtension;
    PWMILIB_CONTEXT         wmilibContext;
    PCHAR                   buffer;
    ULONG                   buffersize;

    PAGED_CODE();

    if (!KdRefreshDebuggerNotPresent())
    {
        KdBreakPoint();
    }

    //
    // Create a filter device object for this device (partition).
    //

    DebugPrint((2, "ScsiChkAddDevice: DriverObject 0x%p DeviceObject 0x%p\n",
            DriverObject, PhysicalDeviceObject));

    status = IoCreateDevice(DriverObject,
                            DEVICE_EXTENSION_SIZE,
                            NULL,
                            FILE_DEVICE_DISK,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &filterDeviceObject);

    if (!NT_SUCCESS(status)) {
       DebugPrint((1, "ScsiChkAddDevice: Cannot create filterDeviceObject\n"));
       return status;
    }

    filterDeviceObject->Flags |= DO_DIRECT_IO;

    deviceExtension = (PDEVICE_EXTENSION) filterDeviceObject->DeviceExtension;

    RtlZeroMemory(deviceExtension, DEVICE_EXTENSION_SIZE);
    KeInitializeSpinLock(&deviceExtension->LogLock);
    ScsiChkGetClock(deviceExtension->LastIdleClock, NULL);
    DebugPrint((10, "ScsiChkAddDevice: LIC=%I64u\n",
                    deviceExtension->LastIdleClock));

    //
    // Allocate per processor counters
    //
    // NOTE: To save memory, we don't allocate memory beyond
    // QueryTime. Remember to expand this size if there is a
    // need to use anything beyond
    //

#if (NTDDI_VERSION >= NTDDI_WIN7)
    deviceExtension->Processors = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
#elif (NTDDI_VERSION >= NTDDI_VISTA)
    deviceExtension->Processors = KeQueryActiveProcessorCount(NULL);
#else
    deviceExtension->Processors = KeNumberProcessors;
#endif

    buffersize= PROCESSOR_COUNTERS_SIZE * deviceExtension->Processors;
    buffer = (PCHAR) ExAllocatePool(NonPagedPool, buffersize);
    if (buffer != NULL) {
        RtlZeroMemory(buffer, buffersize);
        deviceExtension->DiskCounters = (PDISK_PERFORMANCE) buffer;
    }
    else {
        ScsiChkLogError(
            filterDeviceObject,
            513,
            STATUS_SUCCESS,
            IO_ERR_INSUFFICIENT_RESOURCES);
    }

    //
    // Attaches the device object to the highest device object in the chain and
    // return the previously highest device object, which is passed to
    // IoCallDriver when pass IRPs down the device stack
    //

    deviceExtension->PhysicalDeviceObject = PhysicalDeviceObject;

    deviceExtension->TargetDeviceObject =
        IoAttachDeviceToDeviceStack(filterDeviceObject, PhysicalDeviceObject);

    if (deviceExtension->TargetDeviceObject == NULL) {
        ExFreePool(deviceExtension->DiskCounters);
        deviceExtension->DiskCounters = NULL;
        IoDeleteDevice(filterDeviceObject);
        DebugPrint((1, "ScsiChkAddDevice: Unable to attach 0x%p to target 0x%p\n",
            filterDeviceObject, PhysicalDeviceObject));
        return STATUS_NO_SUCH_DEVICE;
    }


    //
    // Initialise the remove lock
    //

    IoInitializeRemoveLock( &deviceExtension->RemoveLock, 'repD', 1, 0 );

    //
    // Save the filter device object in the device extension
    //
    deviceExtension->DeviceObject = filterDeviceObject;

    deviceExtension->PhysicalDeviceName.Buffer
            = deviceExtension->PhysicalDeviceNameBuffer;

    KeInitializeEvent(&deviceExtension->PagingPathCountEvent,
                      NotificationEvent, TRUE);


    //
    // Initialize WMI library context
    //
    wmilibContext = &deviceExtension->WmilibContext;
    RtlZeroMemory(wmilibContext, sizeof(WMILIB_CONTEXT));
    wmilibContext->GuidCount = DiskperfGuidCount;
    wmilibContext->GuidList = DiskperfGuidList;
    wmilibContext->QueryWmiRegInfo = DiskperfQueryWmiRegInfo;
    wmilibContext->QueryWmiDataBlock = DiskperfQueryWmiDataBlock;
    wmilibContext->WmiFunctionControl = DiskperfWmiFunctionControl;

    //
    // default to DO_POWER_PAGABLE
    //

    filterDeviceObject->Flags |=  DO_POWER_PAGABLE;

    //
    // Clear the DO_DEVICE_INITIALIZING flag
    //

    filterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;

} // end ScsiChkAddDevice()


NTSTATUS
ScsiChkDispatchPnp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
/*++

Routine Description:

    Dispatch for PNP

Arguments:

    DeviceObject    - Supplies the device object.

    Irp             - Supplies the I/O request packet.

Return Value:

    NTSTATUS

--*/

{
    PIO_STACK_LOCATION  irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS            status = Irp->IoStatus.Status;
    PDEVICE_EXTENSION   deviceExtension = DeviceObject->DeviceExtension;
    BOOLEAN lockHeld    = FALSE;

    PAGED_CODE();

    DebugPrint((2, "ScsiChkDispatchPnp: DeviceObject 0x%p Irp 0x%p\n",
        DeviceObject, Irp));

    //
    // Acquire the remove lock. If this fails, fail the I/O.
    //

    status = IoAcquireRemoveLock( &deviceExtension->RemoveLock, Irp );

    if ( !NT_SUCCESS(status) ) {

        DebugPrint((2, "IoAcquireRemoveLock failed: DeviceObject %p PNP Irp type [%#02x] %p Status: %!STATUS!.\n",
            DeviceObject, irpSp->MinorFunction, status));
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return status;
    }

    //
    // Indicate that the remove lock is held.
    //

    lockHeld = TRUE;

    switch(irpSp->MinorFunction) {

        case IRP_MN_START_DEVICE:
            //
            // Call the Start Routine handler to schedule a completion routine
            //
            DebugPrint((3,
               "ScsiChkDispatchPnp: Schedule completion for START_DEVICE\n"));
            status = ScsiChkStartDevice(DeviceObject, Irp);
            break;

        case IRP_MN_REMOVE_DEVICE:
        {
            //
            // In this case a completion routine is not required
            // Free resources, pass the IRP down to the next driver
            // Detach and Delete the device.
            //
            DebugPrint((3,
               "ScsiChkDispatchPnp: Processing REMOVE_DEVICE\n"));
            status = ScsiChkRemoveDevice(DeviceObject, Irp);

            //
            // Remove locked released by FpFilterRemoveDevice
            //
            lockHeld = FALSE;

            break;
        }
        case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        {
            PIO_STACK_LOCATION irpStack;
            BOOLEAN setPagable;

            DebugPrint((3,
               "ScsiChkDispatchPnp: Processing DEVICE_USAGE_NOTIFICATION\n"));
            irpStack = IoGetCurrentIrpStackLocation(Irp);

            if (irpStack->Parameters.UsageNotification.Type != DeviceUsageTypePaging) {
                status = ScsiChkSendToNextDriver(DeviceObject, Irp);
                IoReleaseRemoveLock( &deviceExtension->RemoveLock, Irp );
                lockHeld = FALSE;
                break; // out of case statement
            }

            deviceExtension = DeviceObject->DeviceExtension;

            //
            // wait on the paging path event
            //

            status = KeWaitForSingleObject(&deviceExtension->PagingPathCountEvent,
                                           Executive, KernelMode,
                                           FALSE, NULL);

            UNREFERENCED_PARAMETER(status);

            //
            // if removing last paging device, need to set DO_POWER_PAGABLE
            // bit here, and possible re-set it below on failure.
            //

            setPagable = FALSE;
            if (!irpStack->Parameters.UsageNotification.InPath &&
                deviceExtension->PagingPathCount == 1 ) {

                //
                // removing the last paging file
                // must have DO_POWER_PAGABLE bits set
                //

                if (DeviceObject->Flags & DO_POWER_INRUSH) {
                    DebugPrint((3, "ScsiChkDispatchPnp: last paging file "
                                "removed but DO_POWER_INRUSH set, so not "
                                "setting PAGABLE bit "
                                "for DO %p\n", DeviceObject));
                } else {
                    DebugPrint((2, "ScsiChkDispatchPnp: Setting  PAGABLE "
                                "bit for DO %p\n", DeviceObject));
                    DeviceObject->Flags |= DO_POWER_PAGABLE;
                    setPagable = TRUE;
                }

            }

            //
            // send the irp synchronously
            //

            status = ScsiChkForwardIrpSynchronous(DeviceObject, Irp);

            //
            // now deal with the failure and success cases.
            // note that we are not allowed to fail the irp
            // once it is sent to the lower drivers.
            //

            if (NT_SUCCESS(status)) {

                IoAdjustPagingPathCount(
                    &deviceExtension->PagingPathCount,
                    irpStack->Parameters.UsageNotification.InPath);

                if (irpStack->Parameters.UsageNotification.InPath) {
                    if (deviceExtension->PagingPathCount == 1) {

                        //
                        // first paging file addition
                        //

                        DebugPrint((3, "ScsiChkDispatchPnp: Clearing PAGABLE bit "
                                    "for DO %p\n", DeviceObject));
                        DeviceObject->Flags &= ~DO_POWER_PAGABLE;
                    }
                }

            } else {

                //
                // cleanup the changes done above
                //

                if (setPagable == TRUE) {
                    DeviceObject->Flags &= ~DO_POWER_PAGABLE;
                    setPagable = FALSE;
                }

            }

            //
            // set the event so the next one can occur.
            //

            KeSetEvent(&deviceExtension->PagingPathCountEvent,
                       IO_NO_INCREMENT, FALSE);

            //
            // and complete the irp
            //

            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            //
            // Release the remove lock
            //

            IoReleaseRemoveLock( &deviceExtension->RemoveLock, Irp );

            return status;
            break;

        }

        default:
            DebugPrint((3,
               "ScsiChkDispatchPnp: Forwarding irp\n"));

            //
            // Simply forward all other Irps
            //

            status = ScsiChkSendToNextDriver(DeviceObject, Irp);

    }


    //
    // If the lock is still held, release it now.
    //

    if (lockHeld) {

        DebugPrint((2, "ScsiChkDispatchPnp : Releasing Lock: DeviceObject 0x%p Irp 0x%p\n",
        DeviceObject, Irp ));
        //
        // Release the remove lock
        //
        IoReleaseRemoveLock( &deviceExtension->RemoveLock, Irp );
    }

    return status;


} // end ScsiChkDispatchPnp()


NTSTATUS
ScsiChkIrpCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_reads_opt_(_Inexpressible_("varies")) PVOID Context
    )

/*++

Routine Description:

    Forwarded IRP completion routine. Set an event and return
    STATUS_MORE_PROCESSING_REQUIRED. Irp forwarder will wait on this
    event and then re-complete the irp after cleaning up.

Arguments:

    DeviceObject is the device object of the WMI driver
    Irp is the WMI irp that was just completed
    Context is a PKEVENT that forwarder will wait on

Return Value:

    STATUS_MORE_PORCESSING_REQUIRED

--*/

{
    PKEVENT Event = (PKEVENT) Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    if (Event != NULL) {
        KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
    }

    return(STATUS_MORE_PROCESSING_REQUIRED);

} // end ScsiChkIrpCompletion()


NTSTATUS
ScsiChkStartDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
/*++

Routine Description:

    This routine is called when a Pnp Start Irp is received.
    It will schedule a completion routine to initialize and register with WMI.

Arguments:

    DeviceObject - a pointer to the device object

    Irp - a pointer to the irp


Return Value:

    Status of processing the Start Irp

--*/
{
    PDEVICE_EXTENSION   deviceExtension;
    NTSTATUS            status;

    PAGED_CODE();

    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    status = ScsiChkForwardIrpSynchronous(DeviceObject, Irp);

    ScsiChkSyncFilterWithTarget(DeviceObject,
                                 deviceExtension->TargetDeviceObject);

    //
    // Complete WMI registration
    //
    ScsiChkRegisterDevice(DeviceObject);

    //
    // Complete the Irp
    //
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}


NTSTATUS
ScsiChkRemoveDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
/*++

Routine Description:

    This routine is called when the device is to be removed.
    It will de-register itself from WMI first, pass the Irp down the stack
    then detach itself from the stack before deleting itself.

Arguments:

    DeviceObject - a pointer to the device object

    Irp - a pointer to the irp


Return Value:

    Status of removing the device

--*/
{
    NTSTATUS            status;
    PDEVICE_EXTENSION   deviceExtension;
    PWMILIB_CONTEXT     wmilibContext;
    PLOGDATA            logdata;
    HANDLE              logfile;
    OBJECT_ATTRIBUTES   logfile_attrs;
    IO_STATUS_BLOCK     io_status;
    UNICODE_STRING      log_path;

    PAGED_CODE();

    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    RtlInitUnicodeString(&log_path, L"\\SystemRoot\\scsichk.log");
    InitializeObjectAttributes(&logfile_attrs, &log_path, OBJ_CASE_INSENSITIVE|OBJ_OPENIF, NULL, NULL);
    status = ZwCreateFile(&logfile, GENERIC_WRITE, &logfile_attrs, &io_status, NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OVERWRITE_IF,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);

    if (!NT_SUCCESS(status))
    {
        DbgPrint("Error opening log file '%wZ': 0x%X\n", &log_path, status);
        logfile = NULL;
    }

    logdata = deviceExtension->LogData;
    while (logdata != NULL)
    {
        if (NT_SUCCESS(status))
        {
            ULONG length = FIELD_OFFSET(LOGDATA, Data) + logdata->DataLength;
            status = ZwWriteFile(logfile, NULL, NULL, NULL, &io_status, logdata, length, NULL, NULL);

            if (!NT_SUCCESS(status))
            {
                DbgPrint("Error writing log file '%wZ': 0x%X\n", &log_path, status);
                logfile = NULL;
            }
        }

        deviceExtension->LogData = logdata->Next;
        ExFreePool(logdata);
        logdata = deviceExtension->LogData;
    }

    if (logfile != NULL)
    {
        status = ZwClose(logfile);

        if (!NT_SUCCESS(status))
        {
            DbgPrint("Error closing log file '%wZ': 0x%X\n", &log_path, status);
            logfile = NULL;
        }
    }

    //
    // Remove registration with WMI first
    //
    IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_DEREGISTER);

    //
    // quickly zero out the count first to invalid the structure
    //
    wmilibContext = &deviceExtension->WmilibContext;
    InterlockedExchange(
        (PLONG) &(wmilibContext->GuidCount),
        (LONG) 0);
    RtlZeroMemory(wmilibContext, sizeof(WMILIB_CONTEXT));

    if (deviceExtension->DiskCounters) {
        ExFreePool(deviceExtension->DiskCounters);
    }

    //
    // Call Remove lock and wait to ensure all outstanding operations
    // have completed
    //
    IoReleaseRemoveLockAndWait( &deviceExtension->RemoveLock, Irp );

    //
    // Forward the Removal Irp below as per the DDK
    // We aren't required to complete this Irp status should
    // be the return status from the next driver in the stack
    //
    status = ScsiChkSendToNextDriver(DeviceObject, Irp);

    //
    //Detach us from the stack
    //

    IoDetachDevice(deviceExtension->TargetDeviceObject);
    IoDeleteDevice(DeviceObject);

    return status;
}


NTSTATUS
ScsiChkSendToNextDriver(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine sends the Irp to the next driver in line
    when the Irp is not processed by this driver.

Arguments:

    DeviceObject
    Irp

Return Value:

    NTSTATUS

--*/

{
    PDEVICE_EXTENSION   deviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    return IoCallDriver(deviceExtension->TargetDeviceObject, Irp);

} // end ScsiChkSendToNextDriver()

NTSTATUS
ScsiChkDispatchPower(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PDEVICE_EXTENSION deviceExtension;


#if (NTDDI_VERSION < NTDDI_VISTA)
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);

    deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    return PoCallDriver(deviceExtension->TargetDeviceObject, Irp);
#else
    IoSkipCurrentIrpStackLocation(Irp);

    deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    return IoCallDriver(deviceExtension->TargetDeviceObject, Irp);
#endif

} // end ScsiChkDispatchPower

NTSTATUS
ScsiChkForwardIrpSynchronous(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine sends the Irp to the next driver in line
    when the Irp needs to be processed by the lower drivers
    prior to being processed by this one.

Arguments:

    DeviceObject
    Irp

Return Value:

    NTSTATUS

--*/

{
    PDEVICE_EXTENSION   deviceExtension;
    KEVENT event;
    NTSTATUS status;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    //
    // copy the irpstack for the next device
    //

    IoCopyCurrentIrpStackLocationToNext(Irp);

    //
    // set a completion routine
    //

    IoSetCompletionRoutine(Irp, ScsiChkIrpCompletion,
                            &event, TRUE, TRUE, TRUE);

    //
    // call the next lower device
    //

    status = IoCallDriver(deviceExtension->TargetDeviceObject, Irp);

    //
    // wait for the actual completion
    //
__analysis_assume(status != STATUS_PENDING);
__analysis_assume(IoGetCurrentIrpStackLocation(Irp)->MinorFunction != IRP_MN_START_DEVICE);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }

    return status;

} // end ScsiChkForwardIrpSynchronous()


NTSTATUS
ScsiChkCreate(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine services open commands. It establishes
    the driver's existance by returning status success.

Arguments:

    DeviceObject - Context for the activity.
    Irp          - The device control argument block.

Return Value:

    NT Status

--*/

{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;

} // end ScsiChkCreate()


NTSTATUS
ScsiChkReadWrite(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This is the driver entry point for read and write requests
    to disks to which the scsichk driver has attached.
    This driver collects statistics and then sets a completion
    routine so that it can collect additional information when
    the request completes. Then it calls the next driver below
    it.

Arguments:

    DeviceObject
    Irp

Return Value:

    NTSTATUS

--*/

{
    PDEVICE_EXTENSION  deviceExtension   = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION currentIrpStack   = IoGetCurrentIrpStackLocation(Irp);
#if (NTDDI_VERSION >= NTDDI_WIN7)
    ULONG              processor         = KeGetCurrentProcessorNumberEx(NULL);
#else
    ULONG              processor         = KeGetCurrentProcessorNumber();
#endif
    PDISK_PERFORMANCE  partitionCounters = NULL;
    LONG               queueLen;
    PLARGE_INTEGER     timeStamp;
    NTSTATUS           status;

    //
    // As processors may be  dynamically  added, ensure
    // that there's a context for the current processor
    //
    //
    // Acquire the remove lock so that device will not be removed while
    // processing this irp.
    //
    status = IoAcquireRemoveLock(&deviceExtension->RemoveLock, Irp);

    if (!NT_SUCCESS(status))
    {
        DebugPrint((3, "ScsiChkReadWrite: Remove lock failed IOCTL Irp type [%x]\n",
                        currentIrpStack->Parameters.DeviceIoControl.IoControlCode));
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }


    if (deviceExtension->DiskCounters != NULL &&
        processor < deviceExtension->Processors) {

        partitionCounters = (PDISK_PERFORMANCE)
                            ((PCHAR) deviceExtension->DiskCounters
                                + (processor*PROCESSOR_COUNTERS_SIZE));
    }

    //
    // Device is not initialized properly. Blindly pass the irp along
    //
    if (deviceExtension->CountersEnabled <= 0 ||
        deviceExtension->PhysicalDeviceNameBuffer[0] == 0 ||
        partitionCounters == NULL) {
        status = ScsiChkSendToNextDriver(DeviceObject, Irp);
        IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);
        return (status);
    }

    //
    // Increment queue depth counter.
    //

    queueLen = InterlockedIncrement(&deviceExtension->QueueDepth);

    //
    // Copy current stack to next stack.
    //

    IoCopyCurrentIrpStackLocationToNext(Irp);

    //
    // Time stamp current request start.
    //

    timeStamp = (PLARGE_INTEGER) &currentIrpStack->Parameters.Read;
    ScsiChkGetClock(*timeStamp, NULL);
    DebugPrint((10, "ScsiChkReadWrite: TS=%I64u\n", *timeStamp));

    if (queueLen == 1) {
        partitionCounters->IdleTime.QuadPart
            += timeStamp->QuadPart -
                deviceExtension->LastIdleClock.QuadPart;
        deviceExtension->LastIdleClock.QuadPart = timeStamp->QuadPart;
    }

    //
    // Set completion routine callback.
    //

    IoSetCompletionRoutine(Irp,
                           ScsiChkIoCompletion,
                           DeviceObject,
                           TRUE,
                           TRUE,
                           TRUE);

    //
    //
    // Return the results of the call to the disk driver.
    //

    return IoCallDriver(deviceExtension->TargetDeviceObject,
                        Irp);

} // end ScsiChkReadWrite()


NTSTATUS
ScsiChkScsi(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)

/*++

Routine Description:

    This is the driver entry point for read and write requests
    to disks to which the scsichk driver has attached.
    This driver collects statistics and then sets a completion
    routine so that it can collect additional information when
    the request completes. Then it calls the next driver below
    it.

Arguments:

    DeviceObject
    Irp

Return Value:

    NTSTATUS

--*/

{
    PDEVICE_EXTENSION   deviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION  irpStack = IoGetCurrentIrpStackLocation(Irp);
#if (NTDDI_VERSION >= NTDDI_WIN7)
    ULONG               processor = KeGetCurrentProcessorNumberEx(NULL);
#else
    ULONG               processor = KeGetCurrentProcessorNumber();
#endif
    PDISK_PERFORMANCE   partitionCounters = NULL;
    PSCSI_REQUEST_BLOCK srb;
    ULONG               srbFunction;
    ULONG               srbFlags;
    NTSTATUS            status;

    //
    // As processors may be  dynamically  added, ensure
    // that there's a context for the current processor
    //
    //
    // Acquire the remove lock so that device will not be removed while
    // processing this irp.
    //
    status = IoAcquireRemoveLock(&deviceExtension->RemoveLock, Irp);

    if (!NT_SUCCESS(status))
    {
        DebugPrint((3, "ScsiChkScsi: Remove lock failed IOCTL Irp type [%x]\n",
            irpStack->Parameters.DeviceIoControl.IoControlCode));
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }


    if (deviceExtension->DiskCounters != NULL &&
        processor < deviceExtension->Processors) {

        partitionCounters = (PDISK_PERFORMANCE)
            ((PCHAR)deviceExtension->DiskCounters
                + (processor*PROCESSOR_COUNTERS_SIZE));
    }

    //
    // Increment queue depth counter.
    //

    InterlockedIncrement(&deviceExtension->QueueDepth);

    srb = irpStack->Parameters.Scsi.Srb;

    srbFunction = SrbGetSrbFunction(srb);
    srbFlags = SrbGetSrbFlags(srb);

    if (srbFunction == SRB_FUNCTION_EXECUTE_SCSI) {

        PUCHAR pcdb = (PUCHAR)SrbGetCdb(srb);

        switch (pcdb[0])
        {
        case SCSIOP_READ:
        case SCSIOP_READ6:
        case SCSIOP_READ12:
        case SCSIOP_READ16:
        case SCSIOP_WRITE:
        case SCSIOP_WRITE6:
        case SCSIOP_WRITE12:
        case SCSIOP_WRITE16:
            break;

        default:
        {
            ULONG length = SrbGetDataTransferLength(srb);

            DbgPrint("IRP=%#x SCSIOP=%#x length=%u\n", Irp, (int)pcdb[0], length);
        }
        }
    }
    else {
        DbgPrint("IRP=%#x srbFunction=%#x\n", Irp, srbFunction);
    }

    //
    // Copy current stack to next stack.
    //

    IoCopyCurrentIrpStackLocationToNext(Irp);

    //
    // Set completion routine callback.
    //

    IoSetCompletionRoutine(Irp,
        ScsiChkIoCompletion,
        DeviceObject,
        TRUE,
        TRUE,
        TRUE);

    //
    //
    // Return the results of the call to the disk driver.
    //

    return IoCallDriver(deviceExtension->TargetDeviceObject,
        Irp);

} // end ScsiChkReadWrite()


NTSTATUS
ScsiChkIoCompletion(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp,
    IN PVOID          Context
    )

/*++

Routine Description:

    This routine will get control from the system at the completion of an IRP.
    It will calculate the difference between the time the IRP was started
    and the current time, and decrement the queue depth.

Arguments:

    DeviceObject - for the IRP.
    Irp          - The I/O request that just completed.
    Context      - Not used.

Return Value:

    The IRP status.

--*/

{
    PDEVICE_EXTENSION  deviceExtension   = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION irpStack          = IoGetCurrentIrpStackLocation(Irp);
#if (NTDDI_VERSION >= NTDDI_WIN7)
    ULONG              processor         = KeGetCurrentProcessorNumberEx(NULL);
#else
    ULONG              processor         = KeGetCurrentProcessorNumber();
#endif
    PDISK_PERFORMANCE  partitionCounters = NULL;
    LARGE_INTEGER      timeStampComplete;
    PLARGE_INTEGER     difference = NULL;
    LONG               queueLen;

    UNREFERENCED_PARAMETER(Context);

    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }

    ScsiChkGetClock(timeStampComplete, NULL);

    if (irpStack->MajorFunction == IRP_MJ_READ ||
        irpStack->MajorFunction == IRP_MJ_WRITE) {
        //
        // Time stamp current request complete.
        //

        difference = (PLARGE_INTEGER)&irpStack->Parameters.Read;
        difference->QuadPart = timeStampComplete.QuadPart - difference->QuadPart;
        DebugPrint((10, "ScsiChkIoCompletion: TS=%I64u diff %I64u\n",
            timeStampComplete, difference->QuadPart));
    }

    //
    // Decrement the queue depth counters for the volume.  This is
    // done without the spinlock using the Interlocked functions.
    // This is the only
    // legal way to do this.
    //

    queueLen = InterlockedDecrement(&deviceExtension->QueueDepth);

    if (queueLen < 0) { // do not over-decrement. Only happens at start
        queueLen = InterlockedIncrement(&deviceExtension->QueueDepth);
    }
    if (queueLen == 0) {
        deviceExtension->LastIdleClock = timeStampComplete;
    }

    //
    // Update the counters, but only if completion is
    // on a processor that has a preallocated context
    //

    if (deviceExtension->DiskCounters != NULL &&
        processor < deviceExtension->Processors) {

        partitionCounters = (PDISK_PERFORMANCE)
                            ((PCHAR) deviceExtension->DiskCounters
                                + (processor*PROCESSOR_COUNTERS_SIZE));
    }

    if (partitionCounters == NULL) {

        //
        // Release the remove lock
        //
        IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);

        return STATUS_SUCCESS;
    };

    if (irpStack->MajorFunction == IRP_MJ_READ) {

        //
        // Add bytes in this request to bytes read counters.
        //

        partitionCounters->BytesRead.QuadPart += Irp->IoStatus.Information;

        //
        // Increment read requests processed counters.
        //

        partitionCounters->ReadCount++;

        //
        // Calculate request processing time.
        //

        partitionCounters->ReadTime.QuadPart += difference->QuadPart;
        DebugPrint((11, "Added RT delta %I64u total %I64u qlen=%d\n",
            difference->QuadPart, partitionCounters->ReadTime.QuadPart,
            queueLen));
    }

    else if (irpStack->MajorFunction == IRP_MJ_WRITE) {

        //
        // Add bytes in this request to bytes write counters.
        //

        partitionCounters->BytesWritten.QuadPart += Irp->IoStatus.Information;

        //
        // Increment write requests processed counters.
        //

        partitionCounters->WriteCount++;

        //
        // Calculate request processing time.
        //

        partitionCounters->WriteTime.QuadPart += difference->QuadPart;
        DebugPrint((11, "Added WT delta %I64u total %I64u qlen=%d\n",
            difference->QuadPart, partitionCounters->WriteTime.QuadPart,
            queueLen));
    }
    else if (irpStack->MajorFunction == IRP_MJ_SCSI) {

        KIRQL irql = KeGetCurrentIrql();

        PSCSI_REQUEST_BLOCK srb = irpStack->Parameters.Scsi.Srb;
        ULONG data_length = SrbGetDataTransferLength(srb);
        ULONG log_record_length = FIELD_OFFSET(LOGDATA, Data) + data_length;

        PLOGDATA logdata = ExAllocatePool(NonPagedPool, log_record_length);
        ASSERT(logdata != NULL);

        RtlZeroMemory(logdata, log_record_length);

        logdata->Irql = irql;
        logdata->IoStatus = Irp->IoStatus;
        logdata->DataLength = data_length;
        logdata->TimeStamp = timeStampComplete;
        logdata->SrbFunction = SrbGetSrbFunction(srb);
        logdata->ScsiStatus = SrbGetScsiStatus(srb);
        logdata->SenseBufferLength = SrbGetSenseInfoBufferLength(srb);
        logdata->SystemStatus = SrbGetSystemStatus(srb);
        logdata->SrbStatus = SrbGetSrbStatus(srb);

        if (logdata->SenseBufferLength > 0)
        {
            RtlCopyMemory(&logdata->SenseData, SrbGetSenseInfoBuffer(srb),
                min(logdata->SenseBufferLength, sizeof logdata->SenseData));
        }

        if (logdata->DataLength > 0)
        {
            PVOID data_buffer = SrbGetDataBuffer(srb);
            if (data_buffer == NULL && Irp->AssociatedIrp.SystemBuffer != NULL)
            {
                data_buffer = Irp->AssociatedIrp.SystemBuffer;
            }
            if (data_buffer == NULL && Irp->MdlAddress != NULL)
            {
                data_buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
            }

            if (data_buffer != NULL && MmIsAddressValid(data_buffer))
            {
                RtlCopyMemory(logdata->Data, data_buffer, logdata->DataLength);
            }
            else
            {
                DbgPrint("Cannot get data buffer for request.\n");
            }
        }

        if (logdata->SrbFunction == SRB_FUNCTION_EXECUTE_SCSI) {

            logdata->Cdb = *SrbGetCdb(srb);

            switch (logdata->Cdb.CDB6GENERIC.OperationCode)
            {
            case SCSIOP_READ:
            case SCSIOP_READ6:
            case SCSIOP_READ12:
            case SCSIOP_READ16:
            case SCSIOP_WRITE:
            case SCSIOP_WRITE6:
            case SCSIOP_WRITE12:
            case SCSIOP_WRITE16:
                break;

            default:
            {
                DbgPrint("IRQL=%#x IRP=%#x SCSIOP=%#x Length=%u SrbStatus=%#x ScsiStatus=%#x SystemStatus=%#x SenseBufferLength=%#x SenseErrorCode=%#x IoStatus=%#x IoLength=%u\n",
                    irql,
                    Irp, logdata->Cdb.CDB6GENERIC.OperationCode, log_record_length,
                    logdata->SrbStatus, logdata->ScsiStatus, logdata->SystemStatus, logdata->SenseBufferLength,
                    logdata->SenseData.ErrorCode,
                    Irp->IoStatus.Status, (ULONG)Irp->IoStatus.Information);
            }
            }
        }
        else {
            DbgPrint("IRQL=%#x IRP=%#x SrbFunction=%#x Length=%u SrbStatus=%#x ScsiStatus=%#x SystemStatus=%#x SenseBufferLength=%#x SenseErrorCode=%#x IoStatus=%#x IoLength=%u\n",
                irql,
                Irp, logdata->SrbFunction, log_record_length,
                logdata->SrbStatus, logdata->ScsiStatus, logdata->SystemStatus, logdata->SenseBufferLength,
                logdata->SenseData.ErrorCode,
                Irp->IoStatus.Status, (ULONG)Irp->IoStatus.Information);
        }

        if (irql >= DISPATCH_LEVEL)
        {
            KeAcquireSpinLockAtDpcLevel(&deviceExtension->LogLock);
        }
        else
        {
            KeAcquireSpinLock(&deviceExtension->LogLock, &irql);
        }

        logdata->Next = deviceExtension->LogData;
        deviceExtension->LogData = logdata;

        if (irql >= DISPATCH_LEVEL)
        {
            KeReleaseSpinLockFromDpcLevel(&deviceExtension->LogLock);
        }
        else
        {
            KeReleaseSpinLock(&deviceExtension->LogLock, irql);
        }
    }

    if (Irp->Flags & IRP_ASSOCIATED_IRP) {
        partitionCounters->SplitCount++;
    }

    //
    // Release the remove lock
    //
    IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);

    return STATUS_SUCCESS;

} // ScsiChkIoCompletion


NTSTATUS
ScsiChkDeviceControl(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )

/*++

Routine Description:

    This device control dispatcher handles only the disk performance
    device control. All others are passed down to the disk drivers.
    The disk performane device control returns a current snapshot of
    the performance data.

Arguments:

    DeviceObject - Context for the activity.
    Irp          - The device control argument block.

Return Value:

    Status is returned.

--*/

{
    PDEVICE_EXTENSION  deviceExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION currentIrpStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS    status;

    DebugPrint((2, "ScsiChkDeviceControl: DeviceObject 0x%p Irp 0x%p\n",
                    DeviceObject, Irp));


    //
    // Acquire the remove lock so that device will not be removed while
    // processing this irp.
    //
    status = IoAcquireRemoveLock(&deviceExtension->RemoveLock, Irp);

    if (!NT_SUCCESS(status))
    {
        DebugPrint((3, "ScsiChkControl: Remove lock failed IOCTL Irp type [%x]\n",
                 currentIrpStack->Parameters.DeviceIoControl.IoControlCode));
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }


    if (currentIrpStack->Parameters.DeviceIoControl.IoControlCode ==
        IOCTL_DISK_PERFORMANCE) {

        //
        // Verify user buffer is large enough for the performance data.
        //

        if (currentIrpStack->Parameters.DeviceIoControl.OutputBufferLength <
                sizeof(DISK_PERFORMANCE)) {

        //
        // Indicate unsuccessful status and no data transferred.
        //

        status = STATUS_BUFFER_TOO_SMALL;
        Irp->IoStatus.Information = 0;

        }

        else {
            ULONG i;
            PDISK_PERFORMANCE totalCounters;
            PDISK_PERFORMANCE diskCounters = deviceExtension->DiskCounters;
            LARGE_INTEGER frequency, perfctr;

            if (diskCounters == NULL) {
                Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
                //
                // Release the remove lock
                //
                IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_UNSUCCESSFUL;
            }

            if (InterlockedCompareExchange(&deviceExtension->EnabledAlways, 1, 0) == 0)
            {
                InterlockedIncrement(&deviceExtension->CountersEnabled);

                //
                // reset per processor counters only
                //
                if (deviceExtension->DiskCounters != NULL)
                {
                    RtlZeroMemory(deviceExtension->DiskCounters, PROCESSOR_COUNTERS_SIZE * deviceExtension->Processors);
                }

                ScsiChkGetClock(deviceExtension->LastIdleClock, NULL);

                deviceExtension->QueueDepth = 0;

                DebugPrint((10, "ScsiChkDeviceControl: LIC=%I64u\n", deviceExtension->LastIdleClock));
                DebugPrint((3, "ScsiChkDeviceControl: Counters enabled %d\n", deviceExtension->CountersEnabled));
            }

            totalCounters = (PDISK_PERFORMANCE) Irp->AssociatedIrp.SystemBuffer;
            RtlZeroMemory(totalCounters, sizeof(DISK_PERFORMANCE));
#ifdef USE_PERF_CTR
            perfctr = KeQueryPerformanceCounter(&frequency);
#endif
            KeQuerySystemTime(&totalCounters->QueryTime);

            for (i=0; i<deviceExtension->Processors; i++) {
                ScsiChkAddCounters(totalCounters, diskCounters, frequency);
                diskCounters = (PDISK_PERFORMANCE)
                               ((PCHAR) diskCounters + PROCESSOR_COUNTERS_SIZE);
            }
            totalCounters->QueueDepth = deviceExtension->QueueDepth;

            if (totalCounters->QueueDepth == 0) {
                LARGE_INTEGER difference;

                difference.QuadPart =
#ifdef USE_PERF_CTR
                    perfctr.QuadPart
#else
                   totalCounters->QueryTime.QuadPart
#endif
                        - deviceExtension->LastIdleClock.QuadPart;
                if (difference.QuadPart > 0) {
                    totalCounters->IdleTime.QuadPart +=
#ifdef USE_PERF_CTR
                        10000000 * difference.QuadPart / frequency.QuadPart;
#else
                        difference.QuadPart;
#endif
                }
            }
            totalCounters->StorageDeviceNumber
                = deviceExtension->DiskNumber;
            RtlCopyMemory(
                &totalCounters->StorageManagerName[0],
                &deviceExtension->StorageManagerName[0],
                8 * sizeof(WCHAR));
            status = STATUS_SUCCESS;
            Irp->IoStatus.Information = sizeof(DISK_PERFORMANCE);
        }

        //
        // Complete request.
        //

        Irp->IoStatus.Status = status;
        //
        // Release the remove lock
        //
        IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;

    }

    else {

        //
        // Set current stack back one.
        // We aren't doing anything with this IRP
        // so mark it pending
        //

        IoMarkIrpPending(Irp);
        IoSkipCurrentIrpStackLocation(Irp);

        //
        // Pass unrecognized device control requests
        // down to next driver layer. We ignore the
        // return status since we need to return
        // STATUS_PENDING due to the mark pending
        // call above.
        //

        IoCallDriver(deviceExtension->TargetDeviceObject, Irp);

        //
        // Release the remove lock
        //
        IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);

        return (STATUS_PENDING);

    }
} // end ScsiChkDeviceControl()



NTSTATUS ScsiChkWmi(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine handles any WMI requests for information. Since the disk
    information is read-only, is always collected and does not have any
    events only QueryAllData, QuerySingleInstance and GetRegInfo requests
    are supported.

Arguments:

    DeviceObject - Context for the activity.
    Irp          - The device control argument block.

Return Value:

    Status is returned.

--*/

{
    NTSTATUS                status;
    PWMILIB_CONTEXT         wmilibContext;
    SYSCTL_IRP_DISPOSITION  disposition;
    PDEVICE_EXTENSION       deviceExtension = DeviceObject->DeviceExtension;
    BOOLEAN                 lockHeld  = FALSE;

    PAGED_CODE();

    DebugPrint((2, "ScsiChkWmi: DeviceObject 0x%p Irp 0x%p\n",
                    DeviceObject, Irp));
    wmilibContext = &deviceExtension->WmilibContext;

    //
    // Acquire the remove lock. If this fails, fail the I/O.
    //

    status = IoAcquireRemoveLock( &deviceExtension->RemoveLock, Irp );

    if ( !NT_SUCCESS(status) ) {
        DebugPrint((3, "ScsiChkWMI: IoAcquireRemoveLock failed: DeviceObject %p WMI Irp %p Status: %!STATUS!.\n",
            DeviceObject, Irp, status));
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    lockHeld = TRUE;

    if (wmilibContext->GuidCount == 0)  // wmilibContext is not valid
    {
        DebugPrint((3, "ScsiChkWmi: WmilibContext invalid\n"));
        IoReleaseRemoveLock( &deviceExtension->RemoveLock, Irp );
        return ScsiChkSendToNextDriver(DeviceObject, Irp);
    }

    DebugPrint((3, "ScsiChkWmi: Calling WmiSystemControl\n"));
    status = WmiSystemControl(wmilibContext,
                                DeviceObject,
                                Irp,
                                &disposition);
    switch (disposition)
    {
        case IrpProcessed:
        {
            break;
        }

        case IrpNotCompleted:
        {
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            IoReleaseRemoveLock( &deviceExtension->RemoveLock, Irp );
            lockHeld = FALSE;
            break;
        }

//      case IrpForward:
//      case IrpNotWmi:
        default:
        {
            IoReleaseRemoveLock( &deviceExtension->RemoveLock, Irp );
            lockHeld = FALSE;
            status = ScsiChkSendToNextDriver(DeviceObject, Irp);
            break;
        }
    }

    //
    // If the lock is still held, release it now.
    //

    if (lockHeld) {

        DebugPrint((2, "ScsiChkWmi: Releasing Lock: DeviceObject 0x%p Irp 0x%p\n",
        DeviceObject, Irp ));
        //
        // Release the remove lock
        //
        IoReleaseRemoveLock( &deviceExtension->RemoveLock, Irp );
    }

    return(status);
}


NTSTATUS
ScsiChkShutdownFlush(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine is called for a shutdown and flush IRPs.  These are sent by the
    system before it actually shuts down or when the file system does a flush.

Arguments:

    DriverObject - Pointer to device object to being shutdown by system.
    Irp          - IRP involved.

Return Value:

    NT Status

--*/

{
    PDEVICE_EXTENSION  deviceExtension = DeviceObject->DeviceExtension;

    DebugPrint((2, "ScsiChkShutdownFlush: DeviceObject 0x%p Irp 0x%p\n",
                    DeviceObject, Irp));

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(deviceExtension->TargetDeviceObject, Irp);

} // end ScsiChkShutdownFlush()


VOID
ScsiChkUnload(
    IN PDRIVER_OBJECT DriverObject
    )

/*++

Routine Description:

    Free all the allocated resources, etc.

Arguments:

    DriverObject - pointer to a driver object.

Return Value:

    VOID.

--*/
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);

    return;
}


NTSTATUS
ScsiChkRegisterDevice(
    IN PDEVICE_OBJECT DeviceObject
    )

/*++

Routine Description:

    Routine to initialize a proper name for the device object, and
    register it with WMI

Arguments:

    DeviceObject - pointer to a device object to be initialized.

Return Value:

    Status of the initialization. NOTE: If the registration fails,
    the device name in the DeviceExtension will be left as empty.

--*/

{
    NTSTATUS                status;
    IO_STATUS_BLOCK         ioStatus;
    KEVENT                  event;
    PDEVICE_EXTENSION       deviceExtension;
    PIRP                    irp;
    STORAGE_DEVICE_NUMBER   number;
    ULONG                   registrationFlag = 0;

    PAGED_CODE();

    DebugPrint((2, "ScsiChkRegisterDevice: DeviceObject 0x%p\n",
                    DeviceObject));
    deviceExtension = DeviceObject->DeviceExtension;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    //
    // Request for the device number
    //
    irp = IoBuildDeviceIoControlRequest(
                    IOCTL_STORAGE_GET_DEVICE_NUMBER,
                    deviceExtension->TargetDeviceObject,
                    NULL,
                    0,
                    &number,
                    sizeof(number),
                    FALSE,
                    &event,
                    &ioStatus);
    if (!irp) {
        ScsiChkLogError(
            DeviceObject,
            256,
            STATUS_SUCCESS,
            IO_ERR_INSUFFICIENT_RESOURCES);
        DebugPrint((3, "ScsiChkRegisterDevice: Fail to build irp\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(deviceExtension->TargetDeviceObject, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }

    if (NT_SUCCESS(status)) {

        //
        // Remember the disk number for use as parameter in DiskIoNotifyRoutine
        //
        deviceExtension->DiskNumber = number.DeviceNumber;

        //
        // Create device name for each partition
        //

        RtlStringCbPrintfW(
            deviceExtension->PhysicalDeviceNameBuffer,
            sizeof(deviceExtension->PhysicalDeviceNameBuffer),
            L"\\Device\\Harddisk%d\\Partition%d",
            number.DeviceNumber, number.PartitionNumber);

        RtlInitUnicodeString(&deviceExtension->PhysicalDeviceName, &deviceExtension->PhysicalDeviceNameBuffer[0]);

        //
        // Set default name for physical disk
        //
        RtlCopyMemory(
            &(deviceExtension->StorageManagerName[0]),
            L"PhysDisk",
            8 * sizeof(WCHAR));
        DebugPrint((3, "ScsiChkRegisterDevice: Device name %ws\n",
                       deviceExtension->PhysicalDeviceNameBuffer));
    }
    else {

        // request for partition's information failed, try volume

        ULONG           outputSize = sizeof(MOUNTDEV_NAME);
        PMOUNTDEV_NAME  output;
        VOLUME_NUMBER   volumeNumber;
        ULONG           nameSize;

        output = ExAllocatePool(PagedPool, outputSize);
        if (!output) {
            ScsiChkLogError(
                DeviceObject,
                257,
                STATUS_SUCCESS,
                IO_ERR_INSUFFICIENT_RESOURCES);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        KeInitializeEvent(&event, NotificationEvent, FALSE);
        irp = IoBuildDeviceIoControlRequest(
                    IOCTL_MOUNTDEV_QUERY_DEVICE_NAME,
                    deviceExtension->TargetDeviceObject, NULL, 0,
                    output, outputSize, FALSE, &event, &ioStatus);
        if (!irp) {
            ExFreePool(output);
            ScsiChkLogError(
                DeviceObject,
                258,
                STATUS_SUCCESS,
                IO_ERR_INSUFFICIENT_RESOURCES);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        status = IoCallDriver(deviceExtension->TargetDeviceObject, irp);
        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
            status = ioStatus.Status;
        }

        if (status == STATUS_BUFFER_OVERFLOW) {
            outputSize = sizeof(MOUNTDEV_NAME) + output->NameLength;
            ExFreePool(output);
            output = ExAllocatePool(PagedPool, outputSize);

            if (!output) {
                ScsiChkLogError(
                    DeviceObject,
                    258,
                    STATUS_SUCCESS,
                    IO_ERR_INSUFFICIENT_RESOURCES);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            KeInitializeEvent(&event, NotificationEvent, FALSE);
            irp = IoBuildDeviceIoControlRequest(
                        IOCTL_MOUNTDEV_QUERY_DEVICE_NAME,
                        deviceExtension->TargetDeviceObject, NULL, 0,
                        output, outputSize, FALSE, &event, &ioStatus);
            if (!irp) {
                ExFreePool(output);
                ScsiChkLogError(
                    DeviceObject, 259,
                    STATUS_SUCCESS,
                    IO_ERR_INSUFFICIENT_RESOURCES);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            status = IoCallDriver(deviceExtension->TargetDeviceObject, irp);
            if (status == STATUS_PENDING) {
                KeWaitForSingleObject(
                    &event,
                    Executive,
                    KernelMode,
                    FALSE,
                    NULL
                    );
                status = ioStatus.Status;
            }
        }
        if (!NT_SUCCESS(status)) {
            ExFreePool(output);
            ScsiChkLogError(
                DeviceObject,
                260,
                STATUS_SUCCESS,
                IO_ERR_CONFIGURATION_ERROR);
            return status;
        }

        //
        // Since we get the volume name instead of the disk number,
        // set it to a dummy value
        // Todo: Instead of passing the disk number back to the user app.
        // for tracing, pass the STORAGE_DEVICE_NUMBER structure instead.

        deviceExtension->DiskNumber = (ULONG)-1;

        nameSize = min(output->NameLength, sizeof(deviceExtension->PhysicalDeviceNameBuffer) - sizeof(WCHAR));

        RtlStringCbCopyW(deviceExtension->PhysicalDeviceNameBuffer, nameSize, output->Name);

        RtlInitUnicodeString(&deviceExtension->PhysicalDeviceName, &deviceExtension->PhysicalDeviceNameBuffer[0]);

        ExFreePool(output);

        //
        // Now, get the VOLUME_NUMBER information
        //
        outputSize = sizeof(VOLUME_NUMBER);
        RtlZeroMemory(&volumeNumber, outputSize);

        KeInitializeEvent(&event, NotificationEvent, FALSE);
        irp = IoBuildDeviceIoControlRequest(
                 IOCTL_VOLUME_QUERY_VOLUME_NUMBER,
                 deviceExtension->TargetDeviceObject, NULL, 0,
                 &volumeNumber,
                 sizeof(VOLUME_NUMBER),
                 FALSE, &event, &ioStatus);
        if (!irp) {
            ScsiChkLogError(
                DeviceObject,
                265,
                STATUS_SUCCESS,
                IO_ERR_INSUFFICIENT_RESOURCES);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        status = IoCallDriver(deviceExtension->TargetDeviceObject, irp);
        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&event, Executive,
                                  KernelMode, FALSE, NULL);
            status = ioStatus.Status;
        }
        if (!NT_SUCCESS(status) ||
            volumeNumber.VolumeManagerName[0] == (WCHAR) UNICODE_NULL) {

            RtlCopyMemory(
                &deviceExtension->StorageManagerName[0],
                L"LogiDisk",
                8 * sizeof(WCHAR));
            if (NT_SUCCESS(status))
                deviceExtension->DiskNumber = volumeNumber.VolumeNumber;
        }
        else {
            RtlCopyMemory(
                &deviceExtension->StorageManagerName[0],
                &volumeNumber.VolumeManagerName[0],
                8 * sizeof(WCHAR));
            deviceExtension->DiskNumber = volumeNumber.VolumeNumber;
        }
        DebugPrint((3, "ScsiChkRegisterDevice: Device name %ws\n",
                       deviceExtension->PhysicalDeviceNameBuffer));
    }

    status = IoWMIRegistrationControl(DeviceObject,
                     WMIREG_ACTION_REGISTER | registrationFlag );
    if (! NT_SUCCESS(status)) {
        ScsiChkLogError(
            DeviceObject,
            261,
            STATUS_SUCCESS,
            IO_ERR_INTERNAL_ERROR);
    }
    return status;
}


VOID
ScsiChkLogError(
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG UniqueId,
    IN NTSTATUS ErrorCode,
    IN NTSTATUS Status
    )

/*++

Routine Description:

    Routine to log an error with the Error Logger

Arguments:

    DeviceObject - the device object responsible for the error
    UniqueId     - an id for the error
    Status       - the status of the error

Return Value:

    None

--*/

{
    PIO_ERROR_LOG_PACKET errorLogEntry = NULL;
    PVOID* DeviceObjectPtr = NULL;

    //
    // Check to make sure that the Log Entry is large enough to hold the Entry
    // The size of the Log Data Packet cannot be larger than 255 Bytes .
    //

    errorLogEntry = (PIO_ERROR_LOG_PACKET)
                    IoAllocateErrorLogEntry(
                        DeviceObject,
                        (UCHAR)(sizeof(IO_ERROR_LOG_PACKET) + sizeof(PDEVICE_OBJECT))
                        );

    if (errorLogEntry != NULL) {
        errorLogEntry->ErrorCode = ErrorCode;
        errorLogEntry->UniqueErrorValue = UniqueId;
        errorLogEntry->FinalStatus = Status;
        errorLogEntry->DumpDataSize = sizeof(PDEVICE_OBJECT);
        //
        // Log the Device_Object Pointer as the dumpdata
        //
        DeviceObjectPtr = (PVOID*)errorLogEntry->DumpData;
        *DeviceObjectPtr = DeviceObject;

        IoWriteErrorLogEntry(errorLogEntry);
    }
}

NTSTATUS
DiskperfQueryWmiRegInfo(
    IN PDEVICE_OBJECT DeviceObject,
    OUT ULONG *RegFlags,
    OUT PUNICODE_STRING InstanceName,
    OUT PUNICODE_STRING *RegistryPath,
    OUT PUNICODE_STRING MofResourceName,
    OUT PDEVICE_OBJECT *Pdo
    )
/*++

Routine Description:

    This routine is a callback into the driver to retrieve information about
    the guids being registered.

    Implementations of this routine may be in paged memory

Arguments:

    DeviceObject is the device whose registration information is needed

    *RegFlags returns with a set of flags that describe all of the guids being
        registered for this device. If the device wants enable and disable
        collection callbacks before receiving queries for the registered
        guids then it should return the WMIREG_FLAG_EXPENSIVE flag. Also the
        returned flags may specify WMIREG_FLAG_INSTANCE_PDO in which case
        the instance name is determined from the PDO associated with the
        device object. Note that the PDO must have an associated devnode. If
        WMIREG_FLAG_INSTANCE_PDO is not set then Name must return a unique
        name for the device. These flags are ORed into the flags specified
        by the GUIDREGINFO for each guid.

    InstanceName returns with the instance name for the guids if
        WMIREG_FLAG_INSTANCE_PDO is not set in the returned *RegFlags. The
        caller will call ExFreePool with the buffer returned.

    *RegistryPath returns with the registry path of the driver. This is
        required

    MofResourceName returns with the name of the MOF resource attached to
        the binary file. If the driver does not have a mof resource attached
        then this can be returned unmodified. If a value is returned then
        it is NOT freed.

    *Pdo returns with the device object for the PDO associated with this
        device if the WMIREG_FLAG_INSTANCE_PDO flag is retured in
        *RegFlags.

Return Value:

    status
--*/
{
    USHORT size;
    NTSTATUS status;
    PDEVICE_EXTENSION  deviceExtension = DeviceObject->DeviceExtension;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(MofResourceName);

    size = deviceExtension->PhysicalDeviceName.Length + sizeof(UNICODE_NULL);

    InstanceName->Buffer = ExAllocatePool(PagedPool,
                                         size);
    if (InstanceName->Buffer != NULL)
    {
        *RegistryPath = &ScsiChkRegistryPath;

        *RegFlags = WMIREG_FLAG_INSTANCE_PDO | WMIREG_FLAG_EXPENSIVE;
        *Pdo = deviceExtension->PhysicalDeviceObject;
        status = STATUS_SUCCESS;
    } else {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }

    return(status);
}


NTSTATUS
DiskperfQueryWmiDataBlock(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp,
    _In_ ULONG GuidIndex,
    _In_ ULONG InstanceIndex,
    _In_ ULONG InstanceCount,
    _Out_writes_opt_(InstanceCount) PULONG InstanceLengthArray,
    _In_ ULONG BufferAvail,
    _Out_writes_bytes_opt_(BufferAvail) PUCHAR Buffer
    )
/*++

Routine Description:

    This routine is a callback into the driver to query for the contents of
    all instances of a data block. When the driver has finished filling the
    data block it must call WmiCompleteRequest to complete the irp. The
    driver can return STATUS_PENDING if the irp cannot be completed
    immediately.

Arguments:

    DeviceObject is the device whose data block is being queried

    Irp is the Irp that makes this request

    GuidIndex is the index into the list of guids provided when the
        device registered

    InstanceCount is the number of instnaces expected to be returned for
        the data block.

    InstanceLengthArray is a pointer to an array of ULONG that returns the
        lengths of each instance of the data block. If this is NULL then
        there was not enough space in the output buffer to fufill the request
        so the irp should be completed with the buffer needed.

    BufferAvail on entry has the maximum size available to write the data
        blocks.

    Buffer on return is filled with the returned data blocks. Note that each
        instance of the data block must be aligned on a 8 byte boundry.


Return Value:

    status

--*/
{
    NTSTATUS status;
    PDEVICE_EXTENSION deviceExtension;
    ULONG sizeNeeded;
    PDISK_PERFORMANCE totalCounters;
    PDISK_PERFORMANCE diskCounters;
    PWMI_DISK_PERFORMANCE diskPerformance;
    ULONG deviceNameSize;
    PWCHAR diskNamePtr;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(InstanceIndex);
    UNREFERENCED_PARAMETER(InstanceCount);

    deviceExtension = DeviceObject->DeviceExtension;

    if (GuidIndex == 0)
    {
        deviceNameSize = deviceExtension->PhysicalDeviceName.Length +
                         sizeof(USHORT);
        sizeNeeded = ((sizeof(WMI_DISK_PERFORMANCE) + 1) & ~1) +
                                         deviceNameSize;
        diskCounters = deviceExtension->DiskCounters;
        if (diskCounters == NULL)
        {
            status = STATUS_UNSUCCESSFUL;
        }
        else if ((BufferAvail >= sizeNeeded) && (Buffer != NULL))
        {
            //
            // Update idle time if disk has been idle
            //
            ULONG i;
            LARGE_INTEGER perfctr, frequency;

            RtlZeroMemory(Buffer, sizeof(WMI_DISK_PERFORMANCE));
            diskPerformance = (PWMI_DISK_PERFORMANCE)Buffer;

            totalCounters = (PDISK_PERFORMANCE)diskPerformance;
            KeQuerySystemTime(&totalCounters->QueryTime);

#ifdef USE_PERF_CTR
            perfctr = KeQueryPerformanceCounter(&frequency);
#endif
            for (i=0; i<deviceExtension->Processors; i++) {
                ScsiChkAddCounters( totalCounters, diskCounters, frequency);
                DebugPrint((11,
                    "ScsiChkQueryWmiDataBlock: R%d %I64u W%d%I64u ", i,
                    diskCounters->ReadTime, diskCounters->WriteTime));
                diskCounters = (PDISK_PERFORMANCE)
                               ((PCHAR)diskCounters + PROCESSOR_COUNTERS_SIZE);
            }
            DebugPrint((11, "\n"));
            totalCounters->QueueDepth = deviceExtension->QueueDepth;

            DebugPrint((9,
                "QueryWmiDataBlock: Dev %X RT %I64u WT %I64u Rds %d Wts %d freq %I64u\n",
                totalCounters,
                totalCounters->ReadTime, totalCounters->WriteTime,
                totalCounters->ReadCount, totalCounters->WriteCount,
                frequency));

            if (totalCounters->QueueDepth == 0) {
                LARGE_INTEGER difference;

                difference.QuadPart
#ifdef USE_PERF_CTR
                    = perfctr.QuadPart -
#else
                    = totalCounters->QueryTime.QuadPart -
#endif
                           deviceExtension->LastIdleClock.QuadPart;
                if (frequency.QuadPart > 0) {
                    totalCounters->IdleTime.QuadPart +=
#ifdef USE_PERF_CTR
                        10000000 * difference.QuadPart / frequency.QuadPart;
#else
                        difference.QuadPart;
#endif
                }
            }

            totalCounters->StorageDeviceNumber
                = deviceExtension->DiskNumber;
            RtlCopyMemory(
                &totalCounters->StorageManagerName[0],
                &deviceExtension->StorageManagerName[0],
                8 * sizeof(WCHAR));

            diskNamePtr = (PWCHAR)(Buffer +
                          ((sizeof(DISK_PERFORMANCE) + 1) & ~1));
            *diskNamePtr++ = deviceExtension->PhysicalDeviceName.Length;
            RtlCopyMemory(diskNamePtr,
                          deviceExtension->PhysicalDeviceName.Buffer,
                          deviceExtension->PhysicalDeviceName.Length);
            if (InstanceLengthArray) {
                *InstanceLengthArray = sizeNeeded;
            }

            status = STATUS_SUCCESS;
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }

    } else {
        status = STATUS_WMI_GUID_NOT_FOUND;
        sizeNeeded = 0;
    }

    status = WmiCompleteRequest(
                                 DeviceObject,
                                 Irp,
                                 status,
                                 sizeNeeded,
                                 IO_NO_INCREMENT);
    return(status);
}


NTSTATUS
DiskperfWmiFunctionControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN ULONG GuidIndex,
    IN WMIENABLEDISABLECONTROL Function,
    IN BOOLEAN Enable
    )
/*++

Routine Description:

    This routine is a callback into the driver to query for enabling or
    disabling events and data collection.  When the driver has finished it
    must call WmiCompleteRequest to complete the irp. The driver can return
    STATUS_PENDING if the irp cannot be completed immediately.

Arguments:

    DeviceObject is the device whose events or data collection are being
        enabled or disabled

    Irp is the Irp that makes this request

    GuidIndex is the index into the list of guids provided when the
        device registered

    Function differentiates between event and data collection operations

    Enable indicates whether to enable or disable


Return Value:

    status

--*/
{
    NTSTATUS status;
    PDEVICE_EXTENSION deviceExtension;

    deviceExtension = DeviceObject->DeviceExtension;

    if (GuidIndex == 0)
    {
        if (Function == WmiDataBlockControl) {
          if (Enable) {
             if (InterlockedIncrement(&deviceExtension->CountersEnabled) == 1) {
                //
                // Reset per processor counters to 0
                //
                if (deviceExtension->DiskCounters != NULL) {
                    RtlZeroMemory(
                        deviceExtension->DiskCounters,
                        PROCESSOR_COUNTERS_SIZE * deviceExtension->Processors);
                }
                ScsiChkGetClock(deviceExtension->LastIdleClock, NULL);
                DebugPrint((10,
                    "ScsiChkWmiFunctionControl: LIC=%I64u\n",
                    deviceExtension->LastIdleClock));
                deviceExtension->QueueDepth = 0;
                DebugPrint((3, "ScsiChkWmi: Counters enabled %d\n",
                                deviceExtension->CountersEnabled));
             }
          } else {
             if (InterlockedDecrement(&deviceExtension->CountersEnabled)
                  <= 0) {
                deviceExtension->CountersEnabled = 0;
                deviceExtension->QueueDepth = 0;
                DebugPrint((3, "ScsiChkWmi: Counters disabled %d\n",
                                deviceExtension->CountersEnabled));
            }
          }
        }
        status = STATUS_SUCCESS;
    } else {
        status = STATUS_WMI_GUID_NOT_FOUND;
    }

    status = WmiCompleteRequest(
                                 DeviceObject,
                                 Irp,
                                 status,
                                 0,
                                 IO_NO_INCREMENT);
    return(status);
}


VOID
ScsiChkAddCounters(
    IN OUT PDISK_PERFORMANCE TotalCounters,
    IN PDISK_PERFORMANCE NewCounters,
    IN LARGE_INTEGER Frequency
    )
{
    TotalCounters->BytesRead.QuadPart   += NewCounters->BytesRead.QuadPart;
    TotalCounters->BytesWritten.QuadPart+= NewCounters->BytesWritten.QuadPart;
    TotalCounters->ReadCount            += NewCounters->ReadCount;
    TotalCounters->WriteCount           += NewCounters->WriteCount;
    TotalCounters->SplitCount           += NewCounters->SplitCount;
#ifdef USE_PERF_CTR
    if (Frequency.QuadPart > 0) {
        TotalCounters->ReadTime.QuadPart    +=
            NewCounters->ReadTime.QuadPart * 10000000 / Frequency.QuadPart;
        TotalCounters->WriteTime.QuadPart   +=
            NewCounters->WriteTime.QuadPart * 10000000 / Frequency.QuadPart;
        TotalCounters->IdleTime.QuadPart    +=
            NewCounters->IdleTime.QuadPart * 10000000 / Frequency.QuadPart;
    }
    else
#endif
    {
        TotalCounters->ReadTime.QuadPart    += NewCounters->ReadTime.QuadPart;
        TotalCounters->WriteTime.QuadPart   += NewCounters->WriteTime.QuadPart;
        TotalCounters->IdleTime.QuadPart    += NewCounters->IdleTime.QuadPart;
    }
}

#if DBG

VOID
ScsiDebugPrint(
    ULONG DebugPrintLevel,
    PCCHAR DebugMessage,
    ...
)

/*++

Routine Description:

    Debug print for all ScsiChk
    Since this is using the old style of debug prints the following is true :

    Debug levels are bit masks and are not cumulative. So if you want to see
    All errors and warnings you need to have bits 0 and 1 set.

    Mask for DISKPERF is in variable nt!Kd_DISKPERF_Mask

    The Registry can be setup with initial mask value for nt!Kd_DISKPERF_Mask by
    setting up a DWORD value named DISKPERF under key
    HKLM\System\CurrentControlSet\Control\Session Manager\Debug Print Filter

    Alternatively the nt!kd_DEFAULT_mask can be used since all drivers without a
    registry key entry are in the DEFAULT bucket

Arguments:

    Debug print level between 0 and 3, with 3 being the most verbose.

Return Value:

    None

--*/

{
    va_list ap;

    va_start(ap, DebugMessage);

    if ((DebugPrintLevel <= (ScsiChkDebug & 0x0000ffff)) ||
        ((1 << (DebugPrintLevel + 15)) & ScsiChkDebug)) {

        DbgPrint(DebugMessage, ap);
    }

    va_end(ap);

}
#endif

