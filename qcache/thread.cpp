#include "qcache.h"

void
QCacheDeviceWorkerThread(PVOID Context)
{
    auto device_extension = (PDEVICE_EXTENSION)Context;

    for (;;)
    {
        KLOCK_QUEUE_HANDLE lock_handle;
        KIRQL lowest_irql = PASSIVE_LEVEL;

        QCacheAcquireLock(&device_extension->WriteQueueLock, &lock_handle,
            lowest_irql);

        auto request = device_extension->WriteQueue.Flink;

        QCacheReleaseLock(&lock_handle, &lowest_irql);

        if (request == &device_extension->WriteQueue)
        {
            if (device_extension->ShutdownThread)
            {
                break;
            }

            if (!QCacheLinksCreated)
            {
                UNICODE_STRING event_path;
                RtlInitUnicodeString(&event_path,
                    L"\\Device\\" QCACHE_FULL_EVENT_NAME);

                UNICODE_STRING event_link;
                RtlInitUnicodeString(&event_link,
                    L"\\BaseNamedObjects\\Global\\"
                    QCACHE_FULL_EVENT_NAME);

                auto status = IoCreateUnprotectedSymbolicLink(&event_link,
                    &event_path);

                KdPrint((
                    "QCache:DeviceWorkerThread: Link creation status: %#x\n",
                    status));

                if (NT_SUCCESS(status) ||
                    (status == STATUS_OBJECT_NAME_COLLISION))
                {
                    QCacheLinksCreated = true;
                }
            }

            KeWaitForSingleObject(&device_extension->WriteQueueEvent,
                Executive, KernelMode, FALSE, NULL);

            continue;
        }

        auto item = CONTAINING_RECORD(request, WRITE_QUEUE_ITEM, ListEntry);

        QCacheDeferredIrp(device_extension, item);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}
