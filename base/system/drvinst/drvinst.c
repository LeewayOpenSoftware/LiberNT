/*
 * PROJECT:     LiberNT Plug and Play Manager
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Worker process that installs the driver of one device
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/* INCLUDES *****************************************************************/

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <cfgmgr32.h>
#include <umpnpmgr/drvinst.h>

/* The service starts one worker per install so that a hang or a crash of an
 * installer cannot take the service down, and so that it can enforce a time
 * limit. This is the entry point exported by newdev.dll for that purpose. */
BOOL
WINAPI
DevInstallW(
    IN HWND hWndParent,
    IN HINSTANCE hInstance,
    IN LPCWSTR InstanceId,
    IN INT Show);

/* FUNCTIONS ****************************************************************/

static BOOL
ParseHex(
    _In_ PCWSTR String,
    _Out_ ULONG_PTR *Value)
{
    ULONG_PTR Result = 0;
    ULONG Digits = 0;

    for (; *String; String++, Digits++)
    {
        WCHAR c = *String;

        if (c >= L'0' && c <= L'9')
            Result = (Result << 4) | (c - L'0');
        else if (c >= L'a' && c <= L'f')
            Result = (Result << 4) | (c - L'a' + 10);
        else if (c >= L'A' && c <= L'F')
            Result = (Result << 4) | (c - L'A' + 10);
        else
            return FALSE;
    }

    *Value = Result;
    return Digits != 0;
}

static VOID
QueryDevnode(
    _In_ PCWSTR DeviceInstanceId,
    _Inout_ PDRVINST_RESULT Result)
{
    DEVINST DevInst;
    ULONG Status = 0;
    ULONG Problem = 0;

    Result->Cr = CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)DeviceInstanceId, CM_LOCATE_DEVNODE_NORMAL);
    if (Result->Cr != CR_SUCCESS)
        return;

    Result->Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
    if (Result->Cr != CR_SUCCESS)
        return;

    Result->DevStatus = Status;
    Result->Problem = (Status & DN_HAS_PROBLEM) ? Problem : 0;
    if (Status & DN_STARTED)
        Result->Flags |= DRVINST_RF_STARTED;
    if (Status & DN_NEED_RESTART)
        Result->Flags |= DRVINST_RF_NEEDREBOOT;
}

int
wmain(
    int argc,
    WCHAR **argv)
{
    DRVINST_RESULT Result;
    PCWSTR DeviceInstanceId = NULL;
    HANDLE ResultHandle = NULL;
    BOOL ShowWizard = FALSE;
    DWORD Written;
    int i;

    ZeroMemory(&Result, sizeof(Result));
    Result.Size = sizeof(Result);

    for (i = 1; i < argc; i++)
    {
        if (!lstrcmpiW(argv[i], L"/i") && i + 1 < argc)
        {
            DeviceInstanceId = argv[++i];
        }
        else if (!lstrcmpiW(argv[i], L"/h") && i + 1 < argc)
        {
            ULONG_PTR Value;

            if (ParseHex(argv[++i], &Value))
                ResultHandle = (HANDLE)Value;
        }
        else if (!lstrcmpiW(argv[i], L"/w"))
        {
            ShowWizard = TRUE;
        }
    }

    if (!DeviceInstanceId || !DeviceInstanceId[0])
    {
        Result.Win32 = ERROR_INVALID_PARAMETER;
    }
    else
    {
        SetLastError(ERROR_SUCCESS);
        if (!DevInstallW(NULL, NULL, DeviceInstanceId, ShowWizard ? SW_SHOWNOACTIVATE : SW_HIDE))
        {
            Result.Win32 = GetLastError();
            if (Result.Win32 == ERROR_SUCCESS)
                Result.Win32 = ERROR_GEN_FAILURE;
        }

        /* The state of the devnode tells whether the driver also started */
        QueryDevnode(DeviceInstanceId, &Result);
    }

    if (ResultHandle && ResultHandle != INVALID_HANDLE_VALUE)
    {
        WriteFile(ResultHandle, &Result, sizeof(Result), &Written, NULL);
        CloseHandle(ResultHandle);
    }

    return (int)Result.Win32;
}
