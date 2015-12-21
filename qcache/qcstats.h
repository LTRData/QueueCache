
//
// QCache
// ldpstats.h - Kernel/User mode global definitions
//

#ifndef _NTDEF_
typedef LONG NTSTATUS, *PNTSTATUS;
#endif

//
// Tags used for kernel mode allocations and locks.
// Useful with tools like poolmon etc.
//

#define POOL_TAG                    'hcCQ'
#define LOCK_TAG                    'hcCQ'

#define ACCESS_FROM_CTL_CODE(ctrlCode)          ((UCHAR)((ctrlCode >> 14) & 0x03))

//
// Basic name of out-of-memory full event.
//

#define QCACHE_OUT_OF_MEMORY_EVENT_NAME L"QCacheEvent"

//
// Path to out-of-memory event that can be used in calls to OpenEvent.
//

#define QCACHE_OUT_OF_MEMORY_EVENT_PATH L"Global\\" QCACHE_OUT_OF_MEMORY_EVENT_NAME

//
// Driver name and file path
//

#define QCACHE_SERVICE_NAME L"qcache"
#define QCACHE_SERVICE_PATH L"system32\\drivers\\" QCACHE_SERVICE_NAME L".sys"

//
// IOCTL_QCACHE_GET_DEVICE_DATA
//
// This IOCTL is used to request a copy of the DEVICE_STATISTICS object
// that the filter driver is currently using for a filtered device.
//
// Size of output buffer for this request need to be at least
// sizeof(DEVICE_STATISTICS).
//

#define IOCTL_QCACHE_GET_DEVICE_DATA        CTL_CODE(0x8844UL, 0xD01UL, METHOD_BUFFERED, 0)
#define IOCTL_QCACHE_OFF                    CTL_CODE(0x8844UL, 0xD02UL, METHOD_BUFFERED, FILE_READ_ACCESS|FILE_WRITE_ACCESS)
#define IOCTL_QCACHE_ON                     CTL_CODE(0x8844UL, 0xD03UL, METHOD_BUFFERED, FILE_READ_ACCESS|FILE_WRITE_ACCESS)
#define IOCTL_QCACHE_FLUSH                  CTL_CODE(0x8844UL, 0xD04UL, METHOD_BUFFERED, FILE_READ_ACCESS|FILE_WRITE_ACCESS)

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
    // Last NTSTATUS error code while committing lazy-writes or inits
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
    // Number of bytes read from cache queue.
    //
    LONGLONG ReadRequestsFromCache;

    //
    // Number of bytes read from cache queue.
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

FORCEINLINE
CHAR
NextWaitChar(PCHAR chr)
{
    switch (*chr)
    {
    case '\\':
        *chr = '|';
        break;
    case '|':
        *chr = '/';
        break;
    case '/':
        *chr = '-';
        break;
    default:
        *chr = '\\';
        break;
    }

    return *chr;
}

FORCEINLINE
WCHAR
NextWaitCharW(PWCHAR chr)
{
    switch (*chr)
    {
    case L'\\':
        *chr = L'|';
        break;
    case L'|':
        *chr = L'/';
        break;
    case L'/':
        *chr = L'-';
        break;
    default:
        *chr = L'\\';
        break;
    }

    return *chr;
}
