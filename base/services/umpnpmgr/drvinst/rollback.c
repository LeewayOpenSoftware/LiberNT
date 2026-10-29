/*
 * PROJECT:     LiberNT Plug and Play Manager
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver install snapshots and rollback of the device state
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* A snapshot copies the registry keys that describe which driver a device uses:
 * the devnode (Enum\<id>), the class driver key it points to (Class\<guid>\<n>)
 * and the service key (Services\<svc>). Restoring makes the real keys identical
 * to the saved copies, deletes what the failed install added, and re-runs the
 * start so the previous driver binds again. The copies live next to the DrvInst
 * policy in the registry because they are registry metadata; keeping them under
 * Control\DrvInst\Snapshots\<encoded id>\<seq> also survives a service restart,
 * which the in-memory job table does not. */

#define SNAP_VAL_HADDRIVER    L"HadDriver"
#define SNAP_VAL_CLASSKEYPATH L"ClassKeyPath"
#define SNAP_VAL_SERVICENAME  L"ServiceName"
#define SNAP_VAL_DEVICENAME   L"DeviceInstanceId"

#define SNAP_KEY_DEVNODE      L"DevNode"
#define SNAP_KEY_CLASSKEY     L"ClassKey"
#define SNAP_KEY_SERVICE      L"Service"

#define ENUM_KEY              L"SYSTEM\\CurrentControlSet\\Enum\\"
#define CLASS_KEY             L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
#define SERVICES_KEY          L"SYSTEM\\CurrentControlSet\\Services\\"

#define KEY_NAME_CCH          256
#define VALUE_NAME_CCH        256

/* The device id is used as a single registry key name: encode its separators. */
static VOID
EncodeDeviceId(
    _In_ PCWSTR DeviceInstanceId,
    _Out_writes_(DRVINST_DEVICE_ID_CCH) PWSTR Encoded)
{
    for (; *DeviceInstanceId; DeviceInstanceId++)
    {
        *Encoded++ = (*DeviceInstanceId == L'\\') ? L'!' : *DeviceInstanceId;
    }
    *Encoded = UNICODE_NULL;
}

static LONG
OpenPath(
    _In_ HKEY hRoot,
    _In_ PCWSTR Path,
    _In_ REGSAM Sam,
    _Out_ PHKEY phKey)
{
    return RegOpenKeyExW(hRoot, Path, 0, Sam, phKey);
}

/* Copies every value and subkey of hSrc into the already open hDst. */
static LONG
CopyTreeIntoKey(
    _In_ HKEY hSrc,
    _In_ HKEY hDst)
{
    WCHAR ValueName[VALUE_NAME_CCH];
    WCHAR KeyName[KEY_NAME_CCH];
    PBYTE Data = NULL;
    DWORD ValueCount, MaxValueLen, MaxDataLen;
    DWORD KeyCount, MaxKeyLen, Type, Size, i;
    HKEY hSrcSub = NULL, hDstSub = NULL;
    LONG Error;

    Error = RegQueryInfoKeyW(hSrc, NULL, NULL, NULL, &KeyCount, &MaxKeyLen, NULL,
                             &ValueCount, &MaxValueLen, &MaxDataLen, NULL, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (MaxValueLen >= VALUE_NAME_CCH || MaxKeyLen >= KEY_NAME_CCH)
        return ERROR_BUFFER_OVERFLOW;

    for (i = 0; i < ValueCount; i++)
    {
        Size = sizeof(ValueName) / sizeof(ValueName[0]);
        Error = RegEnumValueW(hSrc, i, ValueName, &Size, NULL, &Type, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            return Error;

        if (!Data)
        {
            Data = HeapAlloc(GetProcessHeap(), 0, MaxDataLen ? MaxDataLen : 1);
            if (!Data)
                return ERROR_NOT_ENOUGH_MEMORY;
        }

        Size = MaxDataLen;
        Error = RegQueryValueExW(hSrc, ValueName, NULL, &Type, Data, &Size);
        if (Error != ERROR_SUCCESS)
            break;
        Error = RegSetValueExW(hDst, ValueName, 0, Type, Data, Size);
        if (Error != ERROR_SUCCESS)
            break;
    }

    HeapFree(GetProcessHeap(), 0, Data);

    for (i = 0; KeyCount && i < KeyCount && Error == ERROR_SUCCESS; i++)
    {
        Size = sizeof(KeyName) / sizeof(KeyName[0]);
        Error = RegEnumKeyExW(hSrc, i, KeyName, &Size, NULL, NULL, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            break;

        Error = OpenPath(hSrc, KeyName, KEY_READ, &hSrcSub);
        if (Error != ERROR_SUCCESS)
            break;
        Error = RegCreateKeyExW(hDst, KeyName, 0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_WRITE, NULL, &hDstSub, NULL);
        if (Error == ERROR_SUCCESS)
        {
            Error = CopyTreeIntoKey(hSrcSub, hDstSub);
            RegCloseKey(hDstSub);
        }
        RegCloseKey(hSrcSub);
    }

    return Error;
}

/* Makes the real key identical to the saved snapshot: values and subkeys the
 * snapshot does not have are deleted, the rest is overwritten. */
static LONG
SyncKeyWithSnapshot(
    _In_ HKEY hSnap,
    _In_ HKEY hReal)
{
    WCHAR ValueName[VALUE_NAME_CCH];
    WCHAR KeyName[KEY_NAME_CCH];
    WCHAR SnapName[VALUE_NAME_CCH];
    DWORD ValueCount, MaxValueLen, DataLen;
    DWORD KeyCount, MaxKeyLen, Type, Size, i, j;
    HKEY hSnapSub = NULL, hRealSub = NULL;
    LONG Error;

    Error = RegQueryInfoKeyW(hSnap, NULL, NULL, NULL, &KeyCount, &MaxKeyLen, NULL,
                             &ValueCount, &MaxValueLen, &DataLen, NULL, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (MaxValueLen >= VALUE_NAME_CCH || MaxKeyLen >= KEY_NAME_CCH)
        return ERROR_BUFFER_OVERFLOW;

    for (i = 0; i < ValueCount; i++)
    {
        Size = sizeof(ValueName) / sizeof(ValueName[0]);
        Error = RegEnumValueW(hSnap, i, ValueName, &Size, NULL, &Type, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            return Error;

        Size = 0;
        RegQueryValueExW(hSnap, ValueName, NULL, NULL, NULL, &Size);
        {
            PBYTE Data = HeapAlloc(GetProcessHeap(), 0, Size ? Size : 1);
            if (!Data)
                return ERROR_NOT_ENOUGH_MEMORY;
            Error = RegQueryValueExW(hSnap, ValueName, NULL, NULL, Data, &Size);
            if (Error == ERROR_SUCCESS)
                Error = RegSetValueExW(hReal, ValueName, 0, Type, Data, Size);
            HeapFree(GetProcessHeap(), 0, Data);
        }
        if (Error != ERROR_SUCCESS)
            return Error;
    }

    {
        DWORD RealCount;
        Error = RegQueryInfoKeyW(hReal, NULL, NULL, NULL, NULL, NULL, NULL,
                                 &RealCount, NULL, NULL, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            return Error;

        for (i = 0; i < RealCount; /* requery below as we delete */)
        {
            Size = sizeof(ValueName) / sizeof(ValueName[0]);
            Error = RegEnumValueW(hReal, 0, ValueName, &Size, NULL, NULL, NULL, NULL);
            if (Error != ERROR_SUCCESS)
                return Error == ERROR_NO_MORE_ITEMS ? ERROR_SUCCESS : Error;

            for (j = 0; j < ValueCount; j++)
            {
                Size = sizeof(SnapName) / sizeof(SnapName[0]);
                if (RegEnumValueW(hSnap, j, SnapName, &Size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                    continue;
                if (!lstrcmpW(SnapName, ValueName))
                    break;
            }
            if (j == ValueCount)
            {
                Error = RegDeleteValueW(hReal, ValueName);
                if (Error != ERROR_SUCCESS && Error != ERROR_FILE_NOT_FOUND)
                    return Error;
            }
            i++;
        }
    }

    for (i = 0; i < KeyCount; i++)
    {
        Size = sizeof(KeyName) / sizeof(KeyName[0]);
        Error = RegEnumKeyExW(hSnap, i, KeyName, &Size, NULL, NULL, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            return Error;

        Error = OpenPath(hSnap, KeyName, KEY_READ, &hSnapSub);
        if (Error != ERROR_SUCCESS)
            return Error;
        Error = RegCreateKeyExW(hReal, KeyName, 0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_WRITE | KEY_READ, NULL, &hRealSub, NULL);
        if (Error == ERROR_SUCCESS)
        {
            Error = SyncKeyWithSnapshot(hSnapSub, hRealSub);
            RegCloseKey(hRealSub);
        }
        RegCloseKey(hSnapSub);
        if (Error != ERROR_SUCCESS)
            return Error;
    }

    {
        DWORD RealCount, MaxRealLen;
        Error = RegQueryInfoKeyW(hReal, NULL, NULL, NULL, &RealCount, &MaxRealLen, NULL,
                                 NULL, NULL, NULL, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            return Error;

        for (i = 0; i < RealCount; /* requery as we delete */)
        {
            Size = sizeof(KeyName) / sizeof(KeyName[0]);
            Error = RegEnumKeyExW(hReal, 0, KeyName, &Size, NULL, NULL, NULL, NULL);
            if (Error != ERROR_SUCCESS)
                return Error == ERROR_NO_MORE_ITEMS ? ERROR_SUCCESS : Error;

            if (RegOpenKeyExW(hSnap, KeyName, 0, KEY_READ, &hSnapSub) == ERROR_SUCCESS)
            {
                RegCloseKey(hSnapSub);
            }
            else
            {
                Error = RegDeleteTreeW(hReal, KeyName);
                if (Error != ERROR_SUCCESS && Error != ERROR_FILE_NOT_FOUND)
                    return Error;
            }
            i++;
        }
    }

    return ERROR_SUCCESS;
}

static VOID
PruneOldSnapshots(
    _In_ HKEY hDeviceKey,
    _In_ ULONG KeepSeq)
{
    WCHAR KeyName[KEY_NAME_CCH];
    DWORD KeyCount, MaxKeyLen, Size, i;
    LONG Error;

    Error = RegQueryInfoKeyW(hDeviceKey, NULL, NULL, NULL, &KeyCount, &MaxKeyLen,
                             NULL, NULL, NULL, NULL, NULL, NULL);
    if (Error != ERROR_SUCCESS)
        return;

    for (i = 0; i < KeyCount; i++)
    {
        ULONG Seq;
        Size = sizeof(KeyName) / sizeof(KeyName[0]);
        if (RegEnumKeyExW(hDeviceKey, i, KeyName, &Size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            continue;
        Seq = wcstoul(KeyName, NULL, 10);
        if (Seq < KeepSeq + 1 - DRVINST_MAX_SNAPSHOTS)
            RegDeleteTreeW(hDeviceKey, KeyName);
    }
}

/* Saves the devnode, its class driver key and its service key. */
DWORD
DrvInstSnapshotDevice(
    _In_ PCWSTR DeviceInstanceId,
    _Out_opt_ PULONG OutSeq)
{
    WCHAR Encoded[DRVINST_DEVICE_ID_CCH];
    WCHAR Path[512];
    WCHAR ValueBuffer[VALUE_NAME_CCH];
    HKEY hSnapshots = NULL, hDeviceKey = NULL, hSeqKey = NULL;
    HKEY hSrc = NULL, hDst = NULL;
    DWORD Seq = 0, Size, HadDriver = 0;
    LONG Error;

    if (OutSeq)
        *OutSeq = 0;

    EncodeDeviceId(DeviceInstanceId, Encoded);

    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, DRVINST_REG_SNAPSHOTS, 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hSnapshots, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    Size = sizeof(Seq);
    if (RegQueryValueExW(hSnapshots, L"NextSeq", NULL, NULL, (PBYTE)&Seq, &Size) != ERROR_SUCCESS || Size != sizeof(Seq))
        Seq = 0;
    Seq++;
    RegSetValueExW(hSnapshots, L"NextSeq", 0, REG_DWORD, (PBYTE)&Seq, sizeof(Seq));

    Error = RegCreateKeyExW(hSnapshots, Encoded, 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_ALL_ACCESS, NULL, &hDeviceKey, NULL);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    {
        WCHAR SeqName[16];
        _snwprintf(SeqName, RTL_NUMBER_OF(SeqName), L"%lu", Seq);
        SeqName[RTL_NUMBER_OF(SeqName) - 1] = UNICODE_NULL;
        Error = RegCreateKeyExW(hDeviceKey, SeqName, 0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_ALL_ACCESS, NULL, &hSeqKey, NULL);
    }
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    RegSetValueExW(hSeqKey, SNAP_VAL_DEVICENAME, 0, REG_SZ,
                   (PBYTE)DeviceInstanceId, (lstrlenW(DeviceInstanceId) + 1) * sizeof(WCHAR));

    /* 1. The devnode subtree. */
    _snwprintf(Path, RTL_NUMBER_OF(Path), L"%s%s", ENUM_KEY, DeviceInstanceId);
    Path[RTL_NUMBER_OF(Path) - 1] = UNICODE_NULL;
    Error = OpenPath(HKEY_LOCAL_MACHINE, Path, KEY_READ, &hSrc);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;
    Error = RegCreateKeyExW(hSeqKey, SNAP_KEY_DEVNODE, 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_WRITE, NULL, &hDst, NULL);
    if (Error == ERROR_SUCCESS)
    {
        Error = CopyTreeIntoKey(hSrc, hDst);
        RegCloseKey(hDst);
    }
    RegCloseKey(hSrc);
    hSrc = NULL;
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    /* 2. The class driver key named by the devnode "Driver" value. */
    Size = sizeof(ValueBuffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, Path, L"Driver", RRF_RT_REG_SZ, NULL,
                     ValueBuffer, &Size) == ERROR_SUCCESS)
    {
        WCHAR ClassPath[512];

        _snwprintf(ClassPath, RTL_NUMBER_OF(ClassPath), L"%s%s", CLASS_KEY, ValueBuffer);
        ClassPath[RTL_NUMBER_OF(ClassPath) - 1] = UNICODE_NULL;
        RegSetValueExW(hSeqKey, SNAP_VAL_CLASSKEYPATH, 0, REG_SZ,
                       (PBYTE)ClassPath, (lstrlenW(ClassPath) + 1) * sizeof(WCHAR));
        if (OpenPath(HKEY_LOCAL_MACHINE, ClassPath, KEY_READ, &hSrc) == ERROR_SUCCESS)
        {
            if (RegCreateKeyExW(hSeqKey, SNAP_KEY_CLASSKEY, 0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_WRITE, NULL, &hDst, NULL) == ERROR_SUCCESS)
            {
                if (CopyTreeIntoKey(hSrc, hDst) == ERROR_SUCCESS)
                    HadDriver |= 1;
                RegCloseKey(hDst);
            }
            RegCloseKey(hSrc);
            hSrc = NULL;
        }
    }

    /* 3. The service named by the devnode "Service" value. */
    Size = sizeof(ValueBuffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, Path, L"Service", RRF_RT_REG_SZ, NULL,
                     ValueBuffer, &Size) == ERROR_SUCCESS)
    {
        WCHAR ServicePath[512];

        RegSetValueExW(hSeqKey, SNAP_VAL_SERVICENAME, 0, REG_SZ,
                       (PBYTE)ValueBuffer, (lstrlenW(ValueBuffer) + 1) * sizeof(WCHAR));
        _snwprintf(ServicePath, RTL_NUMBER_OF(ServicePath), L"%s%s", SERVICES_KEY, ValueBuffer);
        ServicePath[RTL_NUMBER_OF(ServicePath) - 1] = UNICODE_NULL;
        if (OpenPath(HKEY_LOCAL_MACHINE, ServicePath, KEY_READ, &hSrc) == ERROR_SUCCESS)
        {
            if (RegCreateKeyExW(hSeqKey, SNAP_KEY_SERVICE, 0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_WRITE, NULL, &hDst, NULL) == ERROR_SUCCESS)
            {
                if (CopyTreeIntoKey(hSrc, hDst) == ERROR_SUCCESS)
                    HadDriver |= 2;
                RegCloseKey(hDst);
            }
            RegCloseKey(hSrc);
            hSrc = NULL;
        }
    }

    RegSetValueExW(hSeqKey, SNAP_VAL_HADDRIVER, 0, REG_DWORD, (PBYTE)&HadDriver, sizeof(HadDriver));

    PruneOldSnapshots(hDeviceKey, Seq);
    if (OutSeq)
        *OutSeq = Seq;
    Error = ERROR_SUCCESS;

Cleanup:
    if (hSrc)
        RegCloseKey(hSrc);
    if (hSeqKey)
        RegCloseKey(hSeqKey);
    if (hDeviceKey)
        RegCloseKey(hDeviceKey);
    if (hSnapshots)
        RegCloseKey(hSnapshots);
    return Error;
}

static ULONG
FindNewestSeq(
    _In_ HKEY hDeviceKey,
    _Out_opt_ PULONG OutCount)
{
    WCHAR KeyName[KEY_NAME_CCH];
    DWORD KeyCount, MaxKeyLen, Size, i;
    ULONG Newest = 0;

    if (OutCount)
        *OutCount = 0;
    if (RegQueryInfoKeyW(hDeviceKey, NULL, NULL, NULL, &KeyCount, &MaxKeyLen, NULL,
                         NULL, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
        return 0;

    for (i = 0; i < KeyCount; i++)
    {
        ULONG Seq;
        Size = sizeof(KeyName) / sizeof(KeyName[0]);
        if (RegEnumKeyExW(hDeviceKey, i, KeyName, &Size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            continue;
        Seq = wcstoul(KeyName, NULL, 10);
        if (OutCount)
            (*OutCount)++;
        if (Seq > Newest)
            Newest = Seq;
    }
    return Newest;
}

/* Restores the newest snapshot of the device and re-runs its start. */
DWORD
DrvInstRollbackDevice(
    _In_ PCWSTR DeviceInstanceId)
{
    WCHAR Encoded[DRVINST_DEVICE_ID_CCH];
    WCHAR Path[512];
    WCHAR CurrentService[VALUE_NAME_CCH];
    WCHAR ClassPath[512];
    WCHAR ServiceName[VALUE_NAME_CCH];
    WCHAR SeqName[16];
    HKEY hSnapshots = NULL, hDeviceKey = NULL, hSeqKey = NULL;
    HKEY hSnap = NULL, hReal = NULL;
    DWORD Size, HadDriver = 0, Disposition;
    ULONG Newest;
    LONG Error;

    EncodeDeviceId(DeviceInstanceId, Encoded);

    Error = OpenPath(HKEY_LOCAL_MACHINE, DRVINST_REG_SNAPSHOTS, KEY_READ, &hSnapshots);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;
    Error = OpenPath(hSnapshots, Encoded, KEY_READ, &hDeviceKey);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    Newest = FindNewestSeq(hDeviceKey, NULL);
    if (!Newest)
    {
        Error = ERROR_NOT_FOUND;
        goto Cleanup;
    }

    _snwprintf(SeqName, RTL_NUMBER_OF(SeqName), L"%lu", Newest);
    SeqName[RTL_NUMBER_OF(SeqName) - 1] = UNICODE_NULL;
    Error = OpenPath(hDeviceKey, SeqName, KEY_READ, &hSeqKey);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    /* The service the failed install registered, before we overwrite the devnode. */
    _snwprintf(Path, RTL_NUMBER_OF(Path), L"%s%s", ENUM_KEY, DeviceInstanceId);
    Path[RTL_NUMBER_OF(Path) - 1] = UNICODE_NULL;
    CurrentService[0] = UNICODE_NULL;
    Size = sizeof(CurrentService);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, Path, L"Service", RRF_RT_REG_SZ, NULL,
                     CurrentService, &Size) != ERROR_SUCCESS)
        CurrentService[0] = UNICODE_NULL;

    Size = sizeof(HadDriver);
    if (RegQueryValueExW(hSeqKey, SNAP_VAL_HADDRIVER, NULL, NULL, (PBYTE)&HadDriver, &Size) != ERROR_SUCCESS)
        HadDriver = 0;

    Size = sizeof(ClassPath);
    if (RegGetValueW(hSeqKey, NULL, SNAP_VAL_CLASSKEYPATH, RRF_RT_REG_SZ, NULL,
                     ClassPath, &Size) != ERROR_SUCCESS)
        ClassPath[0] = UNICODE_NULL;

    ServiceName[0] = UNICODE_NULL;
    Size = sizeof(ServiceName);
    RegGetValueW(hSeqKey, NULL, SNAP_VAL_SERVICENAME, RRF_RT_REG_SZ, NULL, ServiceName, &Size);

    /* 1. Devnode back to the snapshot. */
    Error = OpenPath(hSeqKey, SNAP_KEY_DEVNODE, KEY_READ, &hSnap);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_WRITE | KEY_READ, NULL, &hReal, &Disposition);
    if (Error == ERROR_SUCCESS)
    {
        Error = SyncKeyWithSnapshot(hSnap, hReal);
        RegCloseKey(hReal);
    }
    RegCloseKey(hSnap);
    hSnap = NULL;
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    /* 2. Class driver key: restore the saved one, or delete what the failed install added. */
    if (HadDriver & 1)
    {
        Error = OpenPath(hSeqKey, SNAP_KEY_CLASSKEY, KEY_READ, &hSnap);
        if (Error == ERROR_SUCCESS)
        {
            Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, ClassPath, 0, NULL, REG_OPTION_NON_VOLATILE,
                                    KEY_WRITE | KEY_READ, NULL, &hReal, &Disposition);
            if (Error == ERROR_SUCCESS)
            {
                Error = SyncKeyWithSnapshot(hSnap, hReal);
                RegCloseKey(hReal);
            }
            RegCloseKey(hSnap);
            hSnap = NULL;
        }
    }
    else if (ClassPath[0])
    {
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClassPath);
    }
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    /* 3. Service: restore the saved one, or delete the service of the failed install. */
    if (HadDriver & 2)
    {
        Error = OpenPath(hSeqKey, SNAP_KEY_SERVICE, KEY_READ, &hSnap);
        if (Error == ERROR_SUCCESS)
        {
            WCHAR ServicePath[512];

            _snwprintf(ServicePath, RTL_NUMBER_OF(ServicePath), L"%s%s", SERVICES_KEY, ServiceName);
            ServicePath[RTL_NUMBER_OF(ServicePath) - 1] = UNICODE_NULL;
            Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, ServicePath, 0, NULL, REG_OPTION_NON_VOLATILE,
                                    KEY_WRITE | KEY_READ, NULL, &hReal, &Disposition);
            if (Error == ERROR_SUCCESS)
            {
                Error = SyncKeyWithSnapshot(hSnap, hReal);
                RegCloseKey(hReal);
            }
            RegCloseKey(hSnap);
            hSnap = NULL;
        }
    }
    else if (CurrentService[0])
    {
        WCHAR ServicePath[512];

        _snwprintf(ServicePath, RTL_NUMBER_OF(ServicePath), L"%s%s", SERVICES_KEY, CurrentService);
        ServicePath[RTL_NUMBER_OF(ServicePath) - 1] = UNICODE_NULL;
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, ServicePath);
    }
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    /* 4. Re-run the start so the restored driver binds again. */
    {
        DEVINST DevInst;
        CONFIGRET Cr;

        Cr = CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)DeviceInstanceId, CM_LOCATE_DEVNODE_NORMAL);
        if (Cr == CR_SUCCESS)
            Cr = CM_Setup_DevNode(DevInst, CM_SETUP_DEVNODE_READY);
        if (Cr != CR_SUCCESS)
            DPRINT1("Rollback restart of %ls failed (cr %lu)\n", DeviceInstanceId, Cr);
    }

    Error = ERROR_SUCCESS;

Cleanup:
    if (hSnap)
        RegCloseKey(hSnap);
    if (hSeqKey)
        RegCloseKey(hSeqKey);
    if (hDeviceKey)
        RegCloseKey(hDeviceKey);
    if (hSnapshots)
        RegCloseKey(hSnapshots);
    return Error;
}
