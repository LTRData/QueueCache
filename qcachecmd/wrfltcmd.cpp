#include <winstrct.h>

#include <winioctl.h>
#include <SetupAPI.h>

#include <wio.h>
#include <wreg.hpp>
#include <wsync.h>
#include <wscm.h>

#include "..\qcache\qcstats.h"

#include <iostream>
#include <string>

using namespace std;

#if DBG
#pragma comment(linker, "/nodefaultlib:msvcrt.lib")
#endif

int
stats(int argc, LPWSTR *argv);

int
ioctl(int argc, LPWSTR *argv, DWORD ioctl);

int
wmain(int argc, LPWSTR *argv)
{
    if (argc < 2)
    {
        wcerr <<
            "Example syntax:\n" <<
            argv[0] << " stat C:\n" <<
            argv[0] << " flush C:\n" <<
            argv[0] << " on C:\n" <<
            argv[0] << " off C:" << endl;

        return -1;
    }

    if (_wcsicmp(argv[1], L"stat") == 0)
    {
        return stats(argc - 1, argv + 1);
    }
    else if (_wcsicmp(argv[1], L"flush") == 0)
    {
        return ioctl(argc - 1, argv + 1, IOCTL_QCACHE_FLUSH);
    }
    else if (_wcsicmp(argv[1], L"off") == 0)
    {
        return ioctl(argc - 1, argv + 1, IOCTL_QCACHE_OFF);
    }
    else if (_wcsicmp(argv[1], L"on") == 0)
    {
        return ioctl(argc - 1, argv + 1, IOCTL_QCACHE_ON);
    }
    else
    {
        wcerr << "Unknown command '" << argv[1] << "'.\n" << endl;
        return -1;
    }
}

int
stats(int argc, LPWSTR *argv)
{
    WEvent low_mem_condition(L"Global\\" QCACHE_OUT_OF_MEMORY_EVENT_NAME);
    if (!low_mem_condition)
    {
        win_perror(L"Error getting memory condition state");
    }
    else
    {
        switch (low_mem_condition.Wait(0))
        {
        case WAIT_OBJECT_0:
            wcout << "Low memory condition." << endl;
            break;

        case WAIT_TIMEOUT:
            wcout << "Normal memory condition." << endl;
            break;

        default:
            wcout << "Unknown memory condition." << endl;
        }

        low_mem_condition.Close();
    }

    if (argc > 2)
    {
        wcerr << "Invalid command line parameters." << endl;
        return -1;
    }

    if (argc < 2)
    {
        return 0;
    }

    wcout << endl;

    wstring full_path;

    if (argv[1][0] == L'\\')
    {
        full_path = argv[1];
    }
    else
    {
        full_path = L"\\\\?\\";
        full_path += argv[1];
    }

    WFile device(full_path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, 0, NULL);

    if (!device)
    {
        win_perror(L"Error opening device");
        wcerr << "Device full path: '" << full_path << "'" << endl;
        return 1;
    }

    WHeapMem<DEVICE_STATISTICS> stats(sizeof(DEVICE_STATISTICS),
        HEAP_GENERATE_EXCEPTIONS);

    DWORD bytes;

    if (!DeviceIoControl(
        device.Handle(),
        IOCTL_QCACHE_GET_DEVICE_DATA,
        NULL, 0,
        stats, sizeof(DEVICE_STATISTICS),
        &bytes, NULL))
    {
        win_perror(L"Error querying device");
        wcerr << "Device full path: '" << full_path << "'" << endl;
        return 1;
    }

    device.Close();

    if (stats->Version != sizeof(DEVICE_STATISTICS))
    {
        wcerr << "Wrong version." << endl;
        return 1;
    }

    //printf("Received %u bytes statistics data. Expected: %u\n",
    //    bytes, (DWORD)sizeof(DEVICE_STATISTICS));

    printf(
        "Device is cached:                %s\n"
        "Last error code:                 0x%X\n"
        "\n"
        "Sizes:\n"
        "Size of filtered device:         %.4g %s\n"
        "\n"
        "Statistics:\n"
        "Read requests:                   %I64i\n"
        "Read total:                      %.4g %s\n"
        "Write requests:                  %I64i\n"
        "Written total:                   %.4g %s\n"
        "Write queue items:               %I64i\n"
        "Write queue items top:           %I64i\n"
        "Write queue size:                %.4g %s\n"
        "Write queue size top:            %.4g %s\n"
        "\n"
        "Detailed read statistics:\n"
        "Largest read size:               %.4g %s\n"
        "Read requests direct original:   %I64i\n"
        "Read direct original, total:     %.4g %s\n"
        "Read requests from cache:        %I64i\n"
        "Read requests from cache, total: %.4g %s\n"
        "Split read requests:             %I64i\n"
        "Split read from original, total: %.4g %s\n"
        "\n"
        "Detailed write statistics:\n"
        "Largest write size:              %.4g %s\n"
        "Queued under low-mem condition:  %I64i\n"
        "\n"
        "Page files on filtered device:   %i\n",
        stats->IsCached ? "Yes" : "No",
        stats->LastErrorCode,
        TO_h(stats->Size.QuadPart), TO_p(stats->Size.QuadPart),
        stats->ReadRequests,
        TO_h(stats->ReadBytes), TO_p(stats->ReadBytes),
        stats->WriteRequests,
        TO_h(stats->WrittenBytes), TO_p(stats->WrittenBytes),
        stats->WriteQueueItems,
        stats->WriteQueueItemsTop,
        TO_h(stats->WriteQueueSize), TO_p(stats->WriteQueueSize),
        TO_h(stats->WriteQueueSizeTop), TO_p(stats->WriteQueueSizeTop),
        TO_h(stats->LargestReadSize), TO_p(stats->LargestReadSize),
        stats->ReadRequestsReroutedToOriginal,
        TO_h(stats->ReadBytesReroutedToOriginal),
        TO_p(stats->ReadBytesReroutedToOriginal),
        stats->ReadRequestsFromCache,
        TO_h(stats->ReadBytesFromCache), TO_p(stats->ReadBytesFromCache),
        stats->SplitReads,
        TO_h(stats->ReadBytesFromOriginal), TO_p(stats->ReadBytesFromOriginal),
        TO_h(stats->LargestWriteSize), TO_p(stats->LargestWriteSize),
        stats->LowMemQueued,
        stats->PagingPathCount);

    return 0;
}

int
ioctl(int argc, LPWSTR *argv, DWORD ioctl)
{
    WEvent low_mem_condition(L"Global\\" QCACHE_OUT_OF_MEMORY_EVENT_NAME);
    if (!low_mem_condition)
    {
        win_perror(L"Error getting memory condition state");
    }
    else
    {
        switch (low_mem_condition.Wait(0))
        {
        case WAIT_OBJECT_0:
            wcout << "Low memory condition." << endl;
            break;

        case WAIT_TIMEOUT:
            wcout << "Normal memory condition." << endl;
            break;

        default:
            wcout << "Unknown memory condition." << endl;
        }

        low_mem_condition.Close();
    }

    if (argc > 2)
    {
        wcerr << "Invalid command line parameters." << endl;
        return -1;
    }

    if (argc < 2)
    {
        return 0;
    }

    wcout << endl;

    wstring full_path;

    if (argv[1][0] == L'\\')
    {
        full_path = argv[1];
    }
    else
    {
        full_path = L"\\\\?\\";
        full_path += argv[1];
    }

    DWORD access = 0;
    access |= ACCESS_FROM_CTL_CODE(ioctl) & FILE_READ_ACCESS ?
        GENERIC_READ : 0;
    access |= ACCESS_FROM_CTL_CODE(ioctl) & FILE_WRITE_ACCESS ?
        GENERIC_WRITE : 0;

    WFile device(full_path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if (!device)
    {
        win_perror(L"Error opening device");
        wcerr << "Device full path: '" << full_path << "'" << endl;
        return 1;
    }

    WOverlapped overlapped;

    if (!overlapped)
    {
        win_perror(L"CreateEvent failed");
        return 2;
    }

    cout << "Sending request to device..." << endl;

    DWORD bytes;

    if ((!DeviceIoControl(
        device.Handle(),
        ioctl,
        NULL, 0,
        NULL, 0,
        &bytes, &overlapped)) &&
        GetLastError() != ERROR_IO_PENDING)
    {
        win_perror(L"Error querying device");
        wcerr << "Device full path: '" << full_path << "'" << endl;
        return 1;
    }

    char wait_char = '/';

    while (!overlapped.Wait(200))
    {
        printf("Request sent, waiting for completion... %c\r",
            NextWaitChar(&wait_char));
    }

    printf("                                         \r");

    if (overlapped.GetResult(device.Handle(), &bytes, TRUE))
    {
        puts("Completed successfully.");
    }
    else
    {
        win_perror(L"Device returned error");
    }

    device.Close();

    return 0;
}

class DosDeviceLinkList
{
public:
    class Item
    {
    public:
        wstring Link;
        wstring Target;

        Item *Next;

        Item(LPCWSTR dosdev, LPCWSTR target)
            : Link(dosdev), Target(target), Next(NULL) {}

        ~Item()
        {
            if (Next != NULL)
            {
                delete Next;
                Next = NULL;
            }
        }

    } *First;

    void Clear()
    {
        if (First != NULL)
        {
            delete First;
            First = NULL;
        }
    }

    Item *Add(LPCWSTR dosdev, LPCWSTR target)
    {
        auto item = new Item(dosdev, target);
        auto ptr = &First;
        while (*ptr != NULL)
        {
            ptr = &(*ptr)->Next;
        }
        *ptr = item;
        return item;
    }

    DosDeviceLinkList()
        : First(NULL) {}

    ~DosDeviceLinkList()
    {
        Clear();
    }
};
