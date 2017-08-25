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

#include "qcache.h"

#if DBG
#pragma warning(disable: 28175)
#endif

HANDLE QCacheParametersKey = NULL;
PKEVENT QCacheLowMemCondition = NULL;
PKEVENT QCacheKernelHighMemoryCondition = NULL;
PKEVENT QCacheKernelHighNonPagedPoolCondition = NULL;
PDRIVER_OBJECT QCacheDriverObject = NULL;
bool QCacheLinksCreated = false;
LONGLONG QCacheMaxQueueItems = QCACHE_MAX_QUEUE_ITEMS_DEFAULT_VALUE;
LONGLONG QCacheMaxQueueSize = QCACHE_MAX_QUEUE_SIZE_DEFAULT_VALUE;

//
// Define the sections that allow for discarding (i.e. paging) some of
// the code.
//

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (INIT, QCacheAttachLegacyDevice)
#pragma alloc_text (PAGE, QCacheCreate)
#pragma alloc_text (PAGE, QCacheAddDevice)
#pragma alloc_text (PAGE, QCachePnp)
#pragma alloc_text (PAGE, QCacheStartDevice)
#pragma alloc_text (PAGE, QCacheRemoveDevice)
#pragma alloc_text (PAGE, QCacheCleanupDevice)
#pragma alloc_text (PAGE, QCacheTrim)
#pragma alloc_text (PAGE, QCacheDeviceUsageNotification)
#pragma alloc_text (PAGE, QCacheForwardIrpSynchronous)
#pragma alloc_text (PAGE, QCacheUnload)
#pragma alloc_text (PAGE, QCacheSyncFilterWithTarget)
#endif

NTSTATUS
DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
/*++

Routine Description:

Installable driver initialization entry point.
This entry point is called directly by the I/O manager to set up the disk
performance driver. The driver object is set up and then the Pnp manager
calls QCacheAddDevice to attach to the boot devices.

Arguments:

DriverObject - The disk performance driver object.

RegistryPath - pointer to a unicode string representing the path,
to driver-specific key in the registry.

Return Value:

STATUS_SUCCESS if successful

--*/
{
    QCacheDriverObject = DriverObject;

    NTSTATUS status;
    HANDLE event_handle;

    UNICODE_STRING event_path;
    RtlInitUnicodeString(&event_path,
        L"\\Device\\" QCACHE_OUT_OF_MEMORY_EVENT_NAME);

    KdBreakPoint();

    WPoolMem<SECURITY_DESCRIPTOR, PagedPool> event_security_descriptor(
        SECURITY_DESCRIPTOR_MIN_LENGTH);

    if (event_security_descriptor)
    {
        status = STATUS_SUCCESS;
    }
    else
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        KdPrint(("QCache:DriverEntry: Memory allocation error for security descriptor.\n"));
    }

    if (NT_SUCCESS(status))
    {
        status = RtlCreateSecurityDescriptor(
            event_security_descriptor,
            SECURITY_DESCRIPTOR_REVISION);
    }

    if (NT_SUCCESS(status))
    {
#pragma warning(suppress: 6248)
        status = RtlSetDaclSecurityDescriptor(
            event_security_descriptor,
            TRUE,
            NULL,
            FALSE);
    }

    OBJECT_ATTRIBUTES event_obj_attrs;
    InitializeObjectAttributes(&event_obj_attrs,
        &event_path,
        OBJ_PERMANENT | OBJ_OPENIF,
        NULL,
        event_security_descriptor);

    status = ZwCreateEvent(
        &event_handle,
        EVENT_ALL_ACCESS,
        &event_obj_attrs,
        NotificationEvent,
        FALSE);

    if (!NT_SUCCESS(status))
    {
        DbgPrint("QCache:DriverEntry: Cannot create out-of-memory event '%wZ': %#x\n",
            event_obj_attrs.ObjectName, status);

        KdBreakPoint();
    }
    else
    {
        status = ObReferenceObjectByHandle(event_handle, EVENT_ALL_ACCESS,
            *ExEventObjectType, KernelMode, (PVOID*)&QCacheLowMemCondition,
            NULL);

        if (!NT_SUCCESS(status))
        {
            DbgPrint("QCache:DriverEntry: Cannot reference event '%wZ': %#x\n",
                event_obj_attrs.ObjectName, status);

            QCacheLowMemCondition = NULL;
        }

        ZwClose(event_handle);
    }

    //
    // Open/create memory condition events
    //
    RtlInitUnicodeString(&event_path,
        L"\\KernelObjects\\HighMemoryCondition");

    InitializeObjectAttributes(&event_obj_attrs,
        &event_path,
        OBJ_PERMANENT | OBJ_OPENIF,
        NULL,
        NULL);

    status = ZwCreateEvent(
        &event_handle,
        EVENT_ALL_ACCESS,
        &event_obj_attrs,
        NotificationEvent,
        TRUE);

    if (!NT_SUCCESS(status))
    {
        DbgPrint("QCache:DriverEntry: Cannot create event '%wZ': %#x\n",
            event_obj_attrs.ObjectName, status);

        KdBreakPoint();
    }
    else
    {
        ObReferenceObjectByHandle(event_handle,
            EVENT_ALL_ACCESS,
            *ExEventObjectType,
            KernelMode,
            (PVOID *)&QCacheKernelHighMemoryCondition,
            NULL);

        ZwClose(event_handle);
    }

    RtlInitUnicodeString(&event_path,
        L"\\KernelObjects\\HighNonPagedPoolCondition");

    status = ZwCreateEvent(
        &event_handle,
        EVENT_ALL_ACCESS,
        &event_obj_attrs,
        NotificationEvent,
        TRUE);

    if (!NT_SUCCESS(status))
    {
        DbgPrint("QCache:DriverEntry: Cannot create event '%wZ': %#x\n",
            event_obj_attrs.ObjectName, status);

        KdBreakPoint();
    }
    else
    {
        ObReferenceObjectByHandle(event_handle,
            EVENT_ALL_ACCESS,
            *ExEventObjectType,
            KernelMode,
            (PVOID *)&QCacheKernelHighNonPagedPoolCondition,
            NULL);

        ZwClose(event_handle);
    }

    //
    // Remember registry path
    //

    static const WCHAR parameters_suffix[] = L"\\Parameters";
    
    UNICODE_STRING param_key_path;

    param_key_path.MaximumLength = RegistryPath->Length
        + sizeof(parameters_suffix);

    WPoolMem<WCHAR, PagedPool> param_key_buffer(param_key_path.MaximumLength);

    if (!param_key_buffer)
    {
        KdPrint(("QCache::DriverEntry: Memory allocation error.\n"));

        KdBreakPoint();

        QCacheUnload(DriverObject);

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    param_key_path.Buffer = param_key_buffer;

    RtlCopyUnicodeString(&param_key_path, RegistryPath);
    RtlAppendUnicodeToString(&param_key_path, parameters_suffix);
    param_key_path.Buffer[param_key_path.Length / sizeof(WCHAR)] = 0;

    OBJECT_ATTRIBUTES param_key_obj_attrs;
    InitializeObjectAttributes(&param_key_obj_attrs, &param_key_path,
        OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = ZwOpenKey(&QCacheParametersKey, KEY_READ,
        &param_key_obj_attrs);

    if (!NT_SUCCESS(status))
    {
        DbgPrint("QCache::DriverEntry: Error opening key '%wZ': %#x\n",
            param_key_obj_attrs.ObjectName, status);

        KdBreakPoint();

        QCacheParametersKey = NULL;
    }

    if (QCacheParametersKey != NULL)
    {
        UNICODE_STRING value_name;

        WPoolMem<KEY_VALUE_PARTIAL_INFORMATION, PagedPool>
            reg_value(sizeof(KEY_VALUE_PARTIAL_INFORMATION) +
                sizeof(ULONGLONG));

        if (!reg_value)
        {
            KdPrint(("QCache::DriverEntry: Memory allocation error.\n"));

            KdBreakPoint();

            QCacheUnload(DriverObject);

            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ULONG req_length;

        RtlInitUnicodeString(&value_name, QCACHE_MAX_QUEUE_ITEMS_VALUE_NAME);

        status = ZwQueryValueKey(QCacheParametersKey,
            &value_name, KeyValuePartialInformation, (PVOID)reg_value,
            (ULONG)reg_value.GetSize(), &req_length);

        if (NT_SUCCESS(status))
        {
            QCacheMaxQueueItems = 0;

            RtlCopyMemory(&QCacheMaxQueueItems, reg_value->Data,
                min(reg_value->DataLength, sizeof(QCacheMaxQueueItems)));
        }

        RtlInitUnicodeString(&value_name, QCACHE_MAX_QUEUE_SIZE_VALUE_NAME);

        status = ZwQueryValueKey(QCacheParametersKey,
            &value_name, KeyValuePartialInformation, (PVOID)reg_value,
            (ULONG)reg_value.GetSize(), &req_length);

        if (NT_SUCCESS(status))
        {
            QCacheMaxQueueSize = 0;

            RtlCopyMemory(&QCacheMaxQueueSize, reg_value->Data,
                min(reg_value->DataLength, sizeof(QCacheMaxQueueSize)));
        }
    }

    DbgPrint("QCache: Max queue items = %I64i, Max queue size = %I64i\n",
        QCacheMaxQueueItems, QCacheMaxQueueSize);

    //
    // Create dispatch points
    //

    ULONG ulIndex;
    PDRIVER_DISPATCH *dispatch;
    for (
        ulIndex = 0, dispatch = DriverObject->MajorFunction;
        ulIndex <= IRP_MJ_MAXIMUM_FUNCTION;
        ulIndex++, dispatch++)
    {
        *dispatch = QCacheSendToNextDriver;
    }
    
    //
    // Set up the device driver entry points.
    //

    DriverObject->MajorFunction[IRP_MJ_READ] = QCacheRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = QCacheWrite;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = QCacheDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = QCacheDeviceControl;

    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = QCacheShutdown;

    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = QCacheFlushBuffers;

    DriverObject->MajorFunction[IRP_MJ_PNP] = QCachePnp;

    DriverObject->DriverExtension->AddDevice = QCacheAddDevice;

    DriverObject->DriverUnload = QCacheUnload;

    if (QCacheParametersKey != NULL)
    {
        UNICODE_STRING value_name;

        WPoolMem<KEY_VALUE_PARTIAL_INFORMATION, PagedPool>
            reg_value(sizeof(KEY_VALUE_PARTIAL_INFORMATION) +
                UNICODE_STRING_MAX_BYTES);

        if (!reg_value)
        {
            KdPrint(("QCache::DriverEntry: Memory allocation error.\n"));

            KdBreakPoint();

            QCacheUnload(DriverObject);

            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ULONG req_length;

        RtlInitUnicodeString(&value_name, QCACHE_ATTACH_DEVICES_VALUE_NAME);

        status = ZwQueryValueKey(QCacheParametersKey,
            &value_name, KeyValuePartialInformation, (PVOID)reg_value,
            (ULONG)reg_value.GetSize(), &req_length);

        if (NT_SUCCESS(status))
        {
            UNICODE_STRING device_name;

            device_name.Buffer = (PWCHAR)reg_value->Data;
            device_name.MaximumLength = (USHORT)
                min(MAXUSHORT, reg_value->DataLength);

            while (device_name.MaximumLength >= 6)
            {
                auto len = wcsnlen(device_name.Buffer,
                    device_name.MaximumLength / sizeof(WCHAR));

                device_name.Length = (USHORT)(len * sizeof(WCHAR));

                DbgPrint("QCache: Attaching to device '%wZ'...\n",
                    &device_name);

                auto status = QCacheAttachLegacyDevice(DriverObject, &device_name);

                if (NT_SUCCESS(status))
                {
                    DbgPrint("QCache: Successfully attached to device '%wZ'.\n",
                        &device_name);
                }
                else
                {
                    DbgPrint("QCache: Error attaching to device '%wZ': %#x\n",
                        &device_name, status);

                    KdBreakPoint();

                    QCacheUnload(DriverObject);

                    return status;
                }

                device_name.Buffer += len + 1;

                device_name.MaximumLength -= device_name.Length +
                    sizeof(WCHAR);
            }
        }
    }

    return STATUS_SUCCESS;

}				// end DriverEntry()


NTSTATUS
#pragma warning(suppress: 28101)
QCacheAttachLegacyDevice(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING DeviceName)
{
    PAGED_CODE();

    PDEVICE_OBJECT device_object;
    PFILE_OBJECT file_object;

    auto status = IoGetDeviceObjectPointer(DeviceName, FILE_READ_ATTRIBUTES,
        &file_object, &device_object);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    auto physical_device = IoGetDeviceAttachmentBaseRef(device_object);

    ObDereferenceObject(file_object);

    PDEVICE_EXTENSION filter_device_extension;

    status = QCacheAttachDevice(DriverObject, physical_device,
        &filter_device_extension);

    ObDereferenceObject(physical_device);

    if (NT_SUCCESS(status))
    {
        QCacheInitializeDevice(filter_device_extension);
    }

    return status;
}

#define FILTER_DEVICE_PROPAGATE_FLAGS            0
#define FILTER_DEVICE_PROPAGATE_CHARACTERISTICS (FILE_REMOVABLE_MEDIA |  \
                                                 FILE_READ_ONLY_DEVICE | \
                                                 FILE_FLOPPY_DISKETTE)

VOID
QCacheSyncFilterWithTarget(IN PDEVICE_OBJECT FilterDevice,
IN PDEVICE_OBJECT TargetDevice)
{
    ULONG prop_flags;

    PAGED_CODE();

    //
    // Propagate all useful flags from target to QCache. MountMgr will look
    // at the QCache object capabilities to figure out if the disk is
    // a removable and perhaps other things.
    //

    prop_flags = TargetDevice->Flags & FILTER_DEVICE_PROPAGATE_FLAGS;
    FilterDevice->Flags |= prop_flags;

    prop_flags =
        TargetDevice->Characteristics & FILTER_DEVICE_PROPAGATE_CHARACTERISTICS;
    FilterDevice->Characteristics |= prop_flags;
}

VOID
QCacheCleanupDevice(IN PDEVICE_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    if (DeviceExtension->WorkerThread != NULL)
    {
        DeviceExtension->ShutdownThread = true;
        KeSetEvent(&DeviceExtension->WriteQueueEvent, 0, FALSE);
        ZwWaitForSingleObject(DeviceExtension->WorkerThread, FALSE, NULL);
        ZwClose(DeviceExtension->WorkerThread);
        DeviceExtension->WorkerThread = NULL;
    }

}

NTSTATUS
QCacheSynchronousDeviceControl(
IN PDEVICE_OBJECT DeviceObject,
IN PFILE_OBJECT FileObject,
IN UCHAR MajorFunction,
IN ULONG IoControlCode,
IN OUT PVOID SystemBuffer,
IN ULONG InputBufferLength,
IN ULONG OutputBufferLength,
OUT PIO_STATUS_BLOCK IoStatus)
{
    auto ioctl_irp = IoAllocateIrp(
        DeviceObject->StackSize,
        FALSE);

    if (ioctl_irp == NULL)
    {
        KdBreakPoint();

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ioctl_irp->AssociatedIrp.SystemBuffer = SystemBuffer;

    auto ioctl_stack = IoGetNextIrpStackLocation(ioctl_irp);

    ioctl_stack->MajorFunction = MajorFunction;
    ioctl_stack->FileObject = FileObject;

    ioctl_stack->Parameters.DeviceIoControl.InputBufferLength =
        InputBufferLength;
    ioctl_stack->Parameters.DeviceIoControl.OutputBufferLength =
        OutputBufferLength;
    ioctl_stack->Parameters.DeviceIoControl.IoControlCode =
        IoControlCode;

    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    IoSetCompletionRoutine(ioctl_irp, QCacheSynchronousIrpCompletion,
        &event, TRUE, TRUE, TRUE);

    auto status = IoCallDriver(DeviceObject, ioctl_irp);

    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }

    status = ioctl_irp->IoStatus.Status;

    if (IoStatus != NULL)
    {
        *IoStatus = ioctl_irp->IoStatus;
    }

    QCacheFreeIrpWithMdls(ioctl_irp);

    return status;
}

NTSTATUS
QCacheSynchronousReadWrite(
IN PDEVICE_OBJECT DeviceObject,
IN PFILE_OBJECT FileObject,
IN UCHAR MajorFunction,
IN OUT PVOID SystemBuffer,
IN ULONG BufferLength,
IN PLARGE_INTEGER StartingOffset,
OUT PIO_STATUS_BLOCK IoStatus)
{
    auto ioctl_irp = IoBuildAsynchronousFsdRequest(
        MajorFunction,
        DeviceObject,
        SystemBuffer,
        BufferLength,
        StartingOffset,
        IoStatus);

    if (ioctl_irp == NULL)
    {
        KdBreakPoint();

        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IoGetNextIrpStackLocation(ioctl_irp)->FileObject = FileObject;

    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    IoSetCompletionRoutine(ioctl_irp, QCacheSynchronousIrpCompletion,
        &event, TRUE, TRUE, TRUE);

    auto status = IoCallDriver(DeviceObject, ioctl_irp);

    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }

    status = ioctl_irp->IoStatus.Status;

    if (IoStatus != NULL)
    {
        *IoStatus = ioctl_irp->IoStatus;
    }

    QCacheFreeIrpWithMdls(ioctl_irp);

    return status;
}

NTSTATUS
QCacheInitializeDeviceUnsafe(IN PDEVICE_EXTENSION DeviceExtension)
{
    NTSTATUS status;

    // Query volume size
    if (DeviceExtension->Statistics.Size.QuadPart == 0)
    {
        GET_LENGTH_INFORMATION length;

        status = QCacheSynchronousDeviceControl(
            DeviceExtension->TargetDeviceObject,
            NULL,
            IRP_MJ_DEVICE_CONTROL,
            IOCTL_DISK_GET_LENGTH_INFO,
            &length,
            0,
            sizeof length);

        if (!NT_SUCCESS(status))
        {
            KdPrint(("QCacheInitializeDevice: Error querying volume size: %#x.\n",
                status));

            KdBreakPoint();

            DeviceExtension->Statistics.LastErrorCode = status;

            return status;
        }

        DeviceExtension->Statistics.Size = length.Length;
    }

    status = STATUS_SUCCESS;

    DeviceExtension->Statistics.LastErrorCode = status;

    return status;
}

NTSTATUS
QCacheInitializeDevice(IN PDEVICE_EXTENSION DeviceExtension)
{
    KeAcquireGuardedMutex(&DeviceExtension->InitializationMutex);

    KeWaitForSingleObject(&DeviceExtension->InitializationEvent, Executive,
        KernelMode, FALSE, NULL);

    KeReleaseGuardedMutex(&DeviceExtension->InitializationMutex);

    auto status = QCacheInitializeDeviceUnsafe(DeviceExtension);

    KeSetEvent(&DeviceExtension->InitializationEvent, 0, FALSE);

    return status;
}


NTSTATUS
#pragma warning(suppress: 28152)
QCacheAddDevice(IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject)
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
    PDEVICE_EXTENSION filter_device_extension;
    return QCacheAttachDevice(DriverObject, PhysicalDeviceObject,
        &filter_device_extension);
}


NTSTATUS
#pragma warning(suppress: 28152)
QCacheAttachDevice(IN PDRIVER_OBJECT DriverObject,
IN PDEVICE_OBJECT PhysicalDeviceObject,
OUT PDEVICE_EXTENSION *FilterDeviceExtension)
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
    PAGED_CODE();

    if (PhysicalDeviceObject->Characteristics & FILE_READ_ONLY_DEVICE)
    {
        KdPrint(("QCacheAttachDevice: DeviceObject 0x%p is read-only. Ignored.\n",
            PhysicalDeviceObject));

        return STATUS_SUCCESS;
    }

    NTSTATUS status;

    ULONG req_length;

#if DBG
    
    LONG bus_count = 0;

    WPoolMem<WCHAR, NonPagedPool> ph_obj_name(UNICODE_STRING_MAX_BYTES + 2);
    
    status = IoGetDeviceProperty(PhysicalDeviceObject,
        DevicePropertyEnumeratorName, (ULONG)(ph_obj_name.GetSize() - 2),
        ph_obj_name, &req_length);

    if (!NT_SUCCESS(status))
    {
        DbgPrint("QCache:AddDevice Error getting enumerator name for device: %#x\n",
            status);

        ph_obj_name[0] = 0;
    }

#pragma warning(suppress: 28719)
    wcscat(ph_obj_name, L"\\");

    status = IoGetDeviceProperty(PhysicalDeviceObject,
        DevicePropertyClassName, (ULONG)(ph_obj_name.GetSize() -
        2 * (wcslen(ph_obj_name) + 2)),
        (PWSTR)ph_obj_name + wcslen(ph_obj_name), &req_length);

    if (!NT_SUCCESS(status))
    {
        DbgPrint("QCache:AddDevice Error getting class name for '%ws' device: %#x\n",
            (PWSTR)ph_obj_name, status);
    }

    auto max_chars = (ph_obj_name.GetSize() / 2 - (wcslen(ph_obj_name) + 2));
    
#pragma warning(suppress: 28719)
    auto i = _snwprintf(
        (PWSTR)ph_obj_name + wcslen(ph_obj_name),
        max_chars,
        L"\\%i", bus_count);

    if ((i < 0) || ((ULONG)i >= max_chars))
    {
        DbgPrint("QCache:AddDevice: Enumerator or class names are too long.\n");
    }
    else
    {
        DbgPrint("QCache:AddDevice for Enum\\Class\\Number: '%ws'\n",
            (PWSTR)ph_obj_name);
    }

    ph_obj_name.Free();

    KdBreakPoint();

#endif

    //
    // Create a filter device object for this device (volume).
    //

    PDEVICE_OBJECT filter_device_object;
    status = IoCreateDevice(DriverObject,
        DEVICE_EXTENSION_SIZE,
        NULL,
        FILE_DEVICE_DISK,
        FILE_DEVICE_SECURE_OPEN,
        FALSE, &filter_device_object);

    if (!NT_SUCCESS(status))
    {
        KdPrint((
            "QCacheAttachDevice: Cannot create filter_device_object. Status 0x%X\n",
            status));

        KdBreakPoint();

        return STATUS_SUCCESS;
    }

    *FilterDeviceExtension =
        (PDEVICE_EXTENSION)filter_device_object->DeviceExtension;

    RtlZeroMemory((*FilterDeviceExtension), DEVICE_EXTENSION_SIZE);

    (*FilterDeviceExtension)->Statistics.Version = sizeof(DEVICE_STATISTICS);

    (*FilterDeviceExtension)->Statistics.MaxQueueItems = QCacheMaxQueueItems;

    (*FilterDeviceExtension)->Statistics.MaxQueueSize = QCacheMaxQueueSize;

    //
    // Initialize the remove lock
    //
    IoInitializeRemoveLock(&(*FilterDeviceExtension)->RemoveLock, LOCK_TAG, 1, 0);

    KeInitializeEvent(&(*FilterDeviceExtension)->PagingPathCountEvent,
        SynchronizationEvent, TRUE);
    KeInitializeGuardedMutex(&(*FilterDeviceExtension)->PagingPathCountMutex);

    KeInitializeSpinLock(&(*FilterDeviceExtension)->WriteQueueLock);
    InitializeListHead(&(*FilterDeviceExtension)->WriteQueue);
    KeInitializeEvent(&(*FilterDeviceExtension)->WriteQueueEvent,
        SynchronizationEvent, FALSE);

    KeInitializeEvent(&(*FilterDeviceExtension)->InitializationEvent,
        SynchronizationEvent, TRUE);
    KeInitializeGuardedMutex(&(*FilterDeviceExtension)->InitializationMutex);

    //
    // Save the filter device object in the device extension
    //
    (*FilterDeviceExtension)->DeviceObject = filter_device_object;

    //
    // Attaches the device object to the highest device object in the chain
    // and return the previously highest device object, which is passed to
    // IoCallDriver when pass IRPs down the device stack
    //
    (*FilterDeviceExtension)->PhysicalDeviceObject = PhysicalDeviceObject;

    (*FilterDeviceExtension)->TargetDeviceObject =
        IoAttachDeviceToDeviceStack(filter_device_object,
        PhysicalDeviceObject);

    if ((*FilterDeviceExtension)->TargetDeviceObject == NULL)
    {
        QCacheCleanupDevice((*FilterDeviceExtension));
        IoDeleteDevice(filter_device_object);

        KdPrint((
            "QCacheAttachDevice: Unable to attach 0x%p to target 0x%p\n",
            filter_device_object, PhysicalDeviceObject));

        KdBreakPoint();

        return STATUS_SUCCESS;
    }

    if (((*FilterDeviceExtension)->TargetDeviceObject->Flags & DO_DIRECT_IO) == 0)
    {
        IoDetachDevice((*FilterDeviceExtension)->TargetDeviceObject);

        QCacheCleanupDevice((*FilterDeviceExtension));
        IoDeleteDevice(filter_device_object);

        KdPrint((
            "QCacheAttachDevice: Unable to attach 0x%p to target 0x%p\n",
            filter_device_object, PhysicalDeviceObject));

        KdBreakPoint();

        return STATUS_SUCCESS;
    }

    WPoolMem<OBJECT_NAME_INFORMATION, NonPagedPool> obj_name_info(1024);
    
    status = ObQueryNameString(PhysicalDeviceObject, obj_name_info,
        (ULONG)obj_name_info.GetSize(), &req_length);

    if (NT_SUCCESS(status))
    {
        KdPrint(("QCache:AddDevice for device name '%wZ', driver '%wZ'.\n",
            &obj_name_info->Name,
            &PhysicalDeviceObject->DriverObject->DriverName));
    }

    KdPrint((
        "QCacheAttachDevice: Attached above driver '%wZ'. Filter device flags: %#x Target device flags: %#x Physical device flags: %#x\n",
        &(*FilterDeviceExtension)->TargetDeviceObject->DriverObject->DriverName,
        filter_device_object->Flags,
        (*FilterDeviceExtension)->TargetDeviceObject->Flags,
        PhysicalDeviceObject->Flags));

    status = PsCreateSystemThread(&(*FilterDeviceExtension)->WorkerThread,
        (ACCESS_MASK)0L,
        NULL,
        NULL,
        NULL,
        QCacheDeviceWorkerThread,
        (*FilterDeviceExtension));

    if (!NT_SUCCESS(status))
    {
        (*FilterDeviceExtension)->WorkerThread = NULL;
        IoDetachDevice((*FilterDeviceExtension)->TargetDeviceObject);
        QCacheCleanupDevice((*FilterDeviceExtension));
        IoDeleteDevice(filter_device_object);

        KdBreakPoint();

        return status;
    }

    (*FilterDeviceExtension)->Statistics.IsCached = TRUE;

    //
    // default to DO_POWER_PAGABLE | DO_DIRECT_IO
    //

    filter_device_object->Flags |= DO_POWER_PAGABLE | DO_DIRECT_IO;

    //
    // Clear the DO_DEVICE_INITIALIZING flag
    //

    filter_device_object->Flags &= ~DO_DEVICE_INITIALIZING;
    
    return STATUS_SUCCESS;

}				// end QCacheAddDevice()


NTSTATUS
QCachePnp(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
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
    auto io_stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;
    auto device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    auto lockHeld = false;

    PAGED_CODE();

    //KdPrint(("QCachePnp: DeviceObject 0x%p Irp 0x%p\n",
    //    DeviceObject, Irp));

    //
    // Acquire the remove lock. If this fails, fail the I/O.
    //

    status = IoAcquireRemoveLock(&device_extension->RemoveLock, Irp);

    if (!NT_SUCCESS(status))
    {

        DbgPrint(
            "IoAcquireRemoveLock failed: DeviceObject %p PNP Irp type [%#02x] Status: 0x%X.\n",
            DeviceObject, io_stack->MinorFunction, status);
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        KdBreakPoint();

        return status;
    }

    //
    // Indicate that the remove lock is held.
    //

    lockHeld = true;

    switch (io_stack->MinorFunction)
    {

    case IRP_MN_START_DEVICE:
        //
        // Call the Start Routine handler to schedule a completion routine
        //
        KdPrint((
            "QCachePnp: Schedule completion for START_DEVICE\n"));
        status = QCacheStartDevice(DeviceObject, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
    {
        //
        // In this case a completion routine is not required
        // Free resources, pass the IRP down to the next driver
        // Detach and Delete the device. 
        //
        KdPrint(("QCachePnp: Processing REMOVE_DEVICE\n"));
        status = QCacheRemoveDevice(DeviceObject, Irp);

        //
        // Remove locked released by FpFilterRemoveDevice
        //
        lockHeld = false;

        break;
    }
    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
    {
        KdPrint((
            "QCachePnp: Processing DEVICE_USAGE_NOTIFICATION\n"));

        status = QCacheDeviceUsageNotification(DeviceObject, Irp);

        break;
    }

    default:
        //KdPrint(("QCachePnp: Forwarding irp\n"));

        //
        // Simply forward all other Irps
        //

        status = QCacheSendToNextDriver(DeviceObject, Irp);

    }


    //
    // If the lock is still held, release it now.
    //

    if (lockHeld)
    {

        //DebugPrint((2,
        //    "QCachePnp : Releasing Lock: DeviceObject 0x%p Irp 0x%p\n",
        //    DeviceObject, Irp));
        //
        // Release the remove lock
        //
        IoReleaseRemoveLock(&device_extension->RemoveLock, Irp);
    }

    return status;


}				// end QCachePnp()


NTSTATUS
QCacheSynchronousIrpCompletion(_In_ PDEVICE_OBJECT DeviceObject,
_In_ PIRP Irp,
_In_reads_opt_(_Inexpressible_("varies")) PVOID Context)
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
    auto event = (PKEVENT)Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    if (event != NULL)
    {
        KeSetEvent(event, IO_NO_INCREMENT, FALSE);
    }

    return STATUS_MORE_PROCESSING_REQUIRED;

}				// end QCacheSynchronousIrpCompletion()


NTSTATUS
QCacheStartDevice(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
/*++

Routine Description:

This routine is called when a Pnp Start Irp is received.

Arguments:

DeviceObject - a pointer to the device object

Irp - a pointer to the irp


Return Value:

Status of processing the Start Irp

--*/
{
    PAGED_CODE();

    auto device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    auto status = QCacheForwardIrpSynchronous(DeviceObject, Irp);

    QCacheSyncFilterWithTarget(DeviceObject,
        device_extension->TargetDeviceObject);

    if (device_extension->Statistics.IsCached &&
        (device_extension->Statistics.Size.QuadPart == 0))
    {
        QCacheInitializeDevice(device_extension);
    }

    //
    // Complete the Irp
    //
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}


NTSTATUS
QCacheRemoveDevice(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
/*++

Routine Description:

This routine is called when the device is to be removed.
It will pass the Irp down the stack
then detach itself from the stack before deleting itself.

Arguments:

DeviceObject - a pointer to the device object

Irp - a pointer to the irp


Return Value:

Status of removing the device

--*/
{
    PAGED_CODE();

    auto device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    device_extension->Statistics.IsCached = FALSE;

    QCacheCleanupDevice(device_extension);

    auto status = QCacheSynchronousReadWrite(
        device_extension->TargetDeviceObject,
        NULL,
        IRP_MJ_FLUSH_BUFFERS,
        NULL,
        0,
        NULL,
        NULL);

    if (!NT_SUCCESS(status))
    {
        DbgPrint(
            "QCache:QCacheRemoveDevice: Final flush buffers failed: %#x\n",
            status);
    }

    //
    // Forward the Removal Irp below as per the DDK
    // We aren't required to complete this Irp status should
    // be the return status from the next driver in the stack
    //
    status = QCacheSendToNextDriver(DeviceObject, Irp);

    //
    // Call Remove lock and wait to ensure all outstanding operations
    // have completed
    //
    IoReleaseRemoveLockAndWait(&device_extension->RemoveLock, Irp);

    //
    // Detach us from the stack 
    //
    IoDetachDevice(device_extension->TargetDeviceObject);

    IoDeleteDevice(DeviceObject);

    return status;
}


NTSTATUS
QCacheDeviceUsageNotification(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    PAGED_CODE();

    auto io_stack = IoGetCurrentIrpStackLocation(Irp);

    if ((io_stack->Parameters.UsageNotification.Type !=
        DeviceUsageTypePaging) &&
        (io_stack->Parameters.UsageNotification.Type !=
        DeviceUsageTypeHibernation) &&
        (io_stack->Parameters.UsageNotification.Type !=
        DeviceUsageTypeDumpFile))
    {
        return QCacheSendToNextDriver(DeviceObject, Irp);
    }

    PDEVICE_EXTENSION device_extension =
        (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    //
    // wait on the paging path event
    //

    KeAcquireGuardedMutex(&device_extension->PagingPathCountMutex);

    KeWaitForSingleObject(
        &device_extension->PagingPathCountEvent,
        Executive, KernelMode, FALSE, NULL);

    KeReleaseGuardedMutex(&device_extension->PagingPathCountMutex);

    //
    // if removing last paging device, need to set DO_POWER_PAGABLE
    // bit here, and possible re-set it below on failure.
    //

    auto set_pagable = false;
    if (!io_stack->Parameters.UsageNotification.InPath &&
        device_extension->Statistics.PagingPathCount == 1)
    {

        //
        // removing the last paging file
        // must have DO_POWER_PAGABLE bits set
        //

        if (DeviceObject->Flags & DO_POWER_INRUSH)
        {
            KdPrint(("QCachePnp: last paging file "
                "removed but DO_POWER_INRUSH set, so not "
                "setting PAGABLE bit "
                "for DO %p\n", DeviceObject));
        }
        else
        {
            KdPrint(("QCachePnp: Setting  PAGABLE "
                "bit for DO %p\n", DeviceObject));
            DeviceObject->Flags |= DO_POWER_PAGABLE;
            set_pagable = true;
        }

    }

    //
    // send the irp synchronously
    //

    auto status = QCacheForwardIrpSynchronous(DeviceObject, Irp);

    //
    // now deal with the failure and success cases.
    // note that we are not allowed to fail the irp
    // once it is sent to the lower drivers.
    //

    if (NT_SUCCESS(status))
    {

        IoAdjustPagingPathCount(&device_extension->Statistics.PagingPathCount,
            io_stack->Parameters.UsageNotification.
            InPath);

        if (io_stack->Parameters.UsageNotification.InPath)
        {
            if (device_extension->Statistics.PagingPathCount == 1)
            {

                //
                // first paging file addition
                //

                KdPrint((
                    "QCachePnp: Clearing PAGABLE bit "
                    "for DO %p\n", DeviceObject));
                DeviceObject->Flags &= ~DO_POWER_PAGABLE;
            }
        }

    }
    else
    {
        KdBreakPoint();

        //
        // cleanup the changes done above
        //

        if (set_pagable == true)
        {
            KdPrint((
                "QCachePnp: Lower level driver failed, "
                "resetting PAGABLE bit for DO %p\n", DeviceObject));
            DeviceObject->Flags &= ~DO_POWER_PAGABLE;
            set_pagable = false;
        }

    }

    //
    // set the event so the next one can occur.
    //

    KeSetEvent(&device_extension->PagingPathCountEvent,
        IO_NO_INCREMENT, FALSE);

    //
    // and complete the irp
    //

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}


NTSTATUS
QCacheCreate(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    PAGED_CODE();

    auto device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    if (device_extension->Statistics.IsCached &&
        (device_extension->Statistics.Size.QuadPart == 0))
    {
        if (KeGetCurrentIrql() > APC_LEVEL)
        {
            Irp->IoStatus.Information = 0;
            Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_DEVICE_NOT_READY;
        }

        auto status = QCacheInitializeDevice(device_extension);

        if (!NT_SUCCESS(status))
        {
            KdPrint(("QCacheCreate: Error getting device size: %#x\n",
                status));
        }
    }

    return QCacheSendToNextDriver(DeviceObject, Irp);

}				// end QCacheSendToNextDriver()


NTSTATUS
QCacheSendToNextDriver(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
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
    IoSkipCurrentIrpStackLocation(Irp);

    auto device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    return IoCallDriver(device_extension->TargetDeviceObject, Irp);

}				// end QCacheSendToNextDriver()


NTSTATUS
QCacheForwardIrpSynchronous(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
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
    PAGED_CODE();

    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    auto device_extension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    //
    // copy the irpstack for the next device
    //

    IoCopyCurrentIrpStackLocationToNext(Irp);

    //
    // set a completion routine
    //

    IoSetCompletionRoutine(Irp, QCacheSynchronousIrpCompletion,
        &event, TRUE, TRUE, TRUE);

    //
    // call the next lower device
    //

    auto status = IoCallDriver(device_extension->TargetDeviceObject, Irp);

    //
    // wait for the actual completion
    //
    __analysis_assume(status != STATUS_PENDING);
    __analysis_assume(IoGetCurrentIrpStackLocation(Irp)->MinorFunction !=
        IRP_MN_START_DEVICE);

    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }

    return status;

}				// end QCacheForwardIrpSynchronous()


NTSTATUS
QCacheIgnore(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
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
    UNREFERENCED_PARAMETER(DeviceObject);

    //KdPrint(("QCacheIgnore: DeviceObject 0x%p Irp 0x%p\n",
    //    DeviceObject, Irp));

    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}				// end QCacheIgnore()


VOID
QCacheUnload(IN PDRIVER_OBJECT DriverObject)
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

    if (QCacheParametersKey != NULL)
    {
        ZwClose(QCacheParametersKey);
        QCacheParametersKey = NULL;
    }

    if (QCacheLowMemCondition != NULL)
    {
        ObDereferenceObject(QCacheLowMemCondition);
        QCacheLowMemCondition = NULL;
    }

    return;
}
