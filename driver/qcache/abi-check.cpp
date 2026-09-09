// Compile-time contract with the C# x64 statistics decoder. No runtime changes.
#include <ntifs.h>
#include <stddef.h>
#include "qcstats.h"
static_assert(sizeof(DEVICE_STATISTICS) == 184, "Update the managed statistics ABI");
#define CHECK_OFFSET(field, offset) static_assert(offsetof(DEVICE_STATISTICS, field) == offset, #field)
CHECK_OFFSET(Version, 0);
CHECK_OFFSET(IsCached, 4);
CHECK_OFFSET(LastErrorCode, 8);
CHECK_OFFSET(Size, 16);
CHECK_OFFSET(ReadBytes, 32);
CHECK_OFFSET(WrittenBytes, 104);
CHECK_OFFSET(WriteQueueItems, 120);
CHECK_OFFSET(WriteQueueSize, 136);
CHECK_OFFSET(WriteQueueSizeTop, 144);
CHECK_OFFSET(PagingPathCount, 152);
CHECK_OFFSET(LowMemQueued, 160);
CHECK_OFFSET(MaxQueueItems, 168);
CHECK_OFFSET(MaxQueueSize, 176);
static_assert(IOCTL_QCACHE_GET_DEVICE_DATA == 0x88443404UL, "Update the managed IOCTL");
