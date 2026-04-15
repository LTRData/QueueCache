#pragma once

typedef struct _LOGDATA
{
    struct _LOGDATA *Next;

    LARGE_INTEGER TimeStamp;
    KIRQL Irql;
    IO_STATUS_BLOCK IoStatus;

    ULONG SrbFunction;
    CDB Cdb;
    UCHAR ScsiStatus;
    NTSTATUS SystemStatus;
    UCHAR SrbStatus;

    UCHAR SenseBufferLength;
    SENSE_DATA SenseData;

    ULONG DataLength;
    UCHAR Data[1];

} LOGDATA, *PLOGDATA;

