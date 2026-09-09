#pragma once

// TODO: reference additional headers your program requires here

#define WIN32_LEAN_AND_MEAN
#define _NTSCSI_USER_MODE_

#include <windows.h>
#include <winternl.h>
#include <scsi.h>
#include <vcclr.h>
#include <stdio.h>

typedef UCHAR KIRQL;

#include "..\scsichk\scsichk.h"
