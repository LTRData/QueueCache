#include "stdafx.h"

#using "System.Core.dll"

using namespace System;
using namespace System::IO;
using namespace System::IO::MemoryMappedFiles;
using namespace System::Linq;
using namespace System::Runtime::InteropServices;
using namespace LTRLib::IO;
using namespace LTRLib::LTRGeneric;
using namespace LTRLib::Extensions;

void ShowFileInfo(String ^path);
char *DbgGetScsiOpStr(PCDB pCdb);
char *DbgGetSrbStatusStr(UCHAR SrbStatus);
char *DbgGetSenseCodeStr(UCHAR SrbStatus, PSENSE_DATA senseData);
char *DbgGetAdditionalSenseCodeStr(UCHAR SrbStatus, PSENSE_DATA senseData);
char *DbgGetAdditionalSenseCodeQualifierStr(UCHAR SrbStatus, PSENSE_DATA senseData);

int main(array<String ^> ^args)
{
    for each (auto path in args)
    {
        Console::WriteLine("Parsing log file: '" + path + "'");

        try
        {
            ShowFileInfo(path);
        }
        catch (Exception ^ex)
        {
            Console::ForegroundColor = ConsoleColor::Red;
            Console::Error->WriteLine("Error parsing log file: '" + path + "': " + ex->ToString());
            Console::ResetColor();
        }
    }
}

void ShowFileInfo(String ^path)
{
    DisposableList<IDisposable^> disposable;

    auto file = MemoryMappedFile::CreateFromFile(path, FileMode::Open, nullptr, 0ll, MemoryMappedFileAccess::Read);
    disposable.Add(file);
    auto map = file->CreateViewAccessor(0, 0, MemoryMappedFileAccess::Read);
    disposable.Add(map);

    auto size = map->Capacity;
    PBYTE ptr;
    map->SafeMemoryMappedViewHandle->AcquirePointer(ptr);

    try
    {
        for (auto logdata = (PLOGDATA)ptr;
            (PBYTE)logdata <= ptr + size - sizeof(LOGDATA);
            logdata = (PLOGDATA)(((PBYTE)logdata) + FIELD_OFFSET(LOGDATA, Data) + logdata->DataLength))
        {
            pin_ptr<const wchar_t> timestamp = PtrToStringChars(TimeSpan::FromTicks(logdata->TimeStamp.QuadPart).ToString());

            printf("%ws: IRQL=%#x Op=%s (%#x) SrbStatus=%s (%#x) ScsiStatus=%#x SystemStatus=%#x SenseBufferLength=%#x SenseErrorCode=%#x (%s/%s/%s) IoStatus=%#x IoLength=%u\n",
                (LPCWSTR)timestamp, logdata->Irql,
                logdata->SrbFunction == 0 ? DbgGetScsiOpStr(&logdata->Cdb) : "SrbFunction",
                logdata->SrbFunction == 0 ? logdata->Cdb.CDB6GENERIC.OperationCode : logdata->SrbFunction,
                DbgGetSrbStatusStr(logdata->SrbStatus), logdata->SrbStatus,
                logdata->ScsiStatus,
                logdata->SystemStatus, logdata->SenseBufferLength,
                logdata->SenseData.ErrorCode,
                DbgGetSenseCodeStr(logdata->SrbStatus, &logdata->SenseData), DbgGetAdditionalSenseCodeStr(logdata->SrbStatus, &logdata->SenseData), DbgGetAdditionalSenseCodeQualifierStr(logdata->SrbStatus, &logdata->SenseData),
                logdata->IoStatus.Status, (ULONG)logdata->IoStatus.Information);

            _flushall();

            if (logdata->SrbFunction == 0)
            {
                ULONG length = sizeof CDB;
                while (length > 0 && ((PBYTE)&logdata->Cdb)[length - 1] == 0)
                {
                    length--;
                }
                
                if (length >= 1)
                {
                    auto data = gcnew array<Byte>(length - 1);
                    Marshal::Copy(IntPtr(&logdata->Cdb) + 1, data, 0, length - 1);

                    Console::WriteLine("Raw CDB bytes:");
                    Console::WriteLine(BitConverter::ToString(data));
                }
            }

            if (logdata->DataLength > 0)
            {
                Console::WriteLine("Data transfer size: " + logdata->DataLength.ToString() + " bytes:");

                ULONG length = logdata->DataLength;
                while (length > 0 && logdata->Data[length - 1] == 0)
                {
                    length--;
                }

                auto data = gcnew array<Byte>(length);
                Marshal::Copy(IntPtr(&logdata->Data), data, 0, length);

                if (length == 0)
                {
                    Console::WriteLine("All-zero data.");
                }
                else
                {
                    Console::WriteLine(BitConverter::ToString(data));
                }
            }
        }
    }
    finally
    {
        map->SafeMemoryMappedViewHandle->ReleasePointer();
    }

    return;
}
