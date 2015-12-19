
//
// QCache
// ldpstats.h - Kernel/User mode global definitions
//

#ifndef _NTDEF_
typedef LONG NTSTATUS, *PNTSTATUS;
#endif

//
// Partition type used by default for diff devices.
//

#define QCACHE_PARTITION_TYPE     0x82

//
// Tags used for kernel mode allocations and locks.
// Useful with tools like poolmon etc.
//

#define POOL_TAG                    'caCQ'
#define LOCK_TAG                    'caCQ'

//
// Basic name of diff device full event.
//

#define QCACHE_FULL_EVENT_NAME L"QCacheEvent"

//
// Path to diff device full event that can be used in calls to OpenEvent.
//

#define QCACHE_DIFF_FULL_EVENT_PATH L"Global\\" QCACHE_FULL_EVENT_NAME

//
// Driver name and file path
//

#define QCACHE_SERVICE_NAME L"qcache"
#define QCACHE_SERVICE_PATH L"system32\\drivers\\" QCACHE_SERVICE_NAME L".sys"

//
// IOCTL_QCACHE_GET_DEVICE_DATA
//
// This IOCTL is used to request a copy of the DEVICE_STATISTICS object
// that the filter driver is currenly using for a filtered device.
//
// Size of output buffer for this request need to be at least
// sizeof(DEVICE_STATISTICS).
//

#define IOCTL_QCACHE_GET_DEVICE_DATA         CTL_CODE(0x8844UL, 0xD01UL, METHOD_BUFFERED, 0)

//
// Device statistics
//

typedef struct _DEVICE_STATISTICS
{
    //
    // Version of structure. Set to sizeof(DEVICE_STATISTICS)
    //
    ULONG Version;

    //
    //
    //
    BOOLEAN IsCached;

    //
    // Last NTSTATUS error code if failed to attach a diff device.
    //
    NTSTATUS LastErrorCode;

    //
    // Total size of protected volume in bytes.
    //
    LARGE_INTEGER Size;

    //
    // Number of read requests.
    //
    LONGLONG ReadRequests;

    //
    // Total number of bytes for all read requests.
    //
    LONGLONG ReadBytes;

    //
    // Largest requested read operation.
    //
    ULONG LargestReadSize;

    //
    // Number of read requests redirected to original device.
    //
    LONGLONG ReadRequestsReroutedToOriginal;

    //
    // Total number of bytes for read requests redirected to
    // original device.
    //
    LONGLONG ReadBytesReroutedToOriginal;

    //
    // Number of bytes read from original device in split requests.
    //
    LONGLONG ReadBytesFromOriginal;

    //
    // Number of bytes read from diff device.
    //
    LONGLONG ReadRequestsFromCache;

    //
    // Number of bytes read from diff device.
    //
    LONGLONG ReadBytesFromCache;

    //
    //
    //
    LONGLONG SplitReads;

    //
    // Number of write requests.
    //
    LONGLONG WriteRequests;

    //
    // Total number of bytes written.
    //
    LONGLONG WrittenBytes;

    //
    // Largest requested write operation.
    //
    ULONG LargestWriteSize;

    //
    // 
    //
    LONGLONG WriteQueueItems;

    //
    // 
    //
    LONGLONG WriteQueueItemsTop;

    //
    // 
    //
    LONGLONG WriteQueueSize;

    //
    // 
    //
    LONGLONG WriteQueueSizeTop;

    //
    // Number of paging files, hibernation files and similar at
    // filtered device.
    //
    LONG PagingPathCount;

    //
    //
    //
    LONGLONG LowMemQueued;

} DEVICE_STATISTICS, *PDEVICE_STATISTICS;

