/*
 * PROJECT:     LiberNT Plug and Play Manager
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver install jobs: table, worker process and results
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/* INCLUDES *****************************************************************/

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

/* Finished jobs stay visible until their slot is reused */
#define DRVINST_MAX_JOBS    64

static CRITICAL_SECTION JobLock;
static DRVINST_JOB_INFO JobTable[DRVINST_MAX_JOBS];
static ULONG JobCount;                  /* entries used in JobTable */
static DRVINST_JOB_ID NextJobId = 1;
static HANDLE hRunMutex;                /* one install at a time */
static BOOL JobsInitialized;

/* TABLE ********************************************************************/

VOID
DrvInstInitialize(VOID)
{
    InitializeCriticalSection(&JobLock);
    hRunMutex = CreateMutexW(NULL, FALSE, NULL);
    JobsInitialized = (hRunMutex != NULL);
}

static PDRVINST_JOB_INFO
FindJob(
    _In_ DRVINST_JOB_ID JobId)
{
    ULONG i;

    for (i = 0; i < JobCount; i++)
    {
        if (JobTable[i].JobId == JobId)
            return &JobTable[i];
    }

    return NULL;
}

/* The caller holds JobLock. Reuse the oldest finished slot when the table is full. */
static PDRVINST_JOB_INFO
AllocateJob(VOID)
{
    PDRVINST_JOB_INFO Oldest = NULL;
    ULONG i;

    if (JobCount < DRVINST_MAX_JOBS)
        return &JobTable[JobCount++];

    for (i = 0; i < JobCount; i++)
    {
        if (JobTable[i].State < DrvInstJobSucceeded)
            continue;
        if (!Oldest || JobTable[i].JobId < Oldest->JobId)
            Oldest = &JobTable[i];
    }

    return Oldest;
}

static BOOL
IsValidDeviceInstanceId(
    _In_ PCWSTR DeviceInstanceId)
{
    static const WCHAR Allowed[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789\\&_.#{}()-+";
    PCWSTR p;

    for (; *DeviceInstanceId; DeviceInstanceId++)
    {
        for (p = Allowed; *p; p++)
        {
            if (*p == *DeviceInstanceId)
                break;
        }
        if (!*p)
            return FALSE;
    }
    return TRUE;
}

static DRVINST_JOB_ID
CreateJob(
    _In_ PCWSTR DeviceInstanceId,
    _In_ ULONG Source,
    _In_ ULONG Flags)
{
    PDRVINST_JOB_INFO Job;
    DRVINST_JOB_ID JobId = 0;

    if (!DeviceInstanceId[0] || !IsValidDeviceInstanceId(DeviceInstanceId))
        return 0;

    EnterCriticalSection(&JobLock);

    Job = AllocateJob();
    if (Job)
    {
        ZeroMemory(Job, sizeof(*Job));
        Job->Size = sizeof(*Job);
        Job->JobId = JobId = NextJobId++;
        Job->State = DrvInstJobQueued;
        Job->Source = Source;
        Job->Flags = Flags;
        Job->SubmitTick = GetTickCount();
        lstrcpynW(Job->DeviceInstanceId, DeviceInstanceId, DRVINST_DEVICE_ID_CCH);
    }

    LeaveCriticalSection(&JobLock);

    return JobId;
}

static VOID
SetJobState(
    _In_ DRVINST_JOB_ID JobId,
    _In_ ULONG State,
    _In_opt_ const DRVINST_RESULT *Result)
{
    PDRVINST_JOB_INFO Job;
    ULONG Tick;

    EnterCriticalSection(&JobLock);

    Job = FindJob(JobId);
    if (Job)
    {
        Job->State = State;
        if (State >= DrvInstJobSucceeded)
        {
            Tick = GetTickCount();
            Job->EndTick = Tick ? Tick : 1;
        }
        if (Result)
            Job->Result = *Result;
    }

    LeaveCriticalSection(&JobLock);
}

BOOL
DrvInstGetJobInfo(
    _In_ DRVINST_JOB_ID JobId,
    _Out_ PDRVINST_JOB_INFO Info)
{
    PDRVINST_JOB_INFO Job;
    BOOL Found = FALSE;

    if (!JobsInitialized)
        return FALSE;

    EnterCriticalSection(&JobLock);

    Job = FindJob(JobId);
    if (Job)
    {
        *Info = *Job;
        Found = TRUE;
    }

    LeaveCriticalSection(&JobLock);

    return Found;
}

/* Copies up to Max jobs, newest first, and returns how many were copied */
ULONG
DrvInstpEnumJobs(
    _Out_writes_to_(Max, return) PDRVINST_JOB_INFO Info,
    _In_ ULONG Max)
{
    ULONG Count = 0;
    DRVINST_JOB_ID Wanted;

    if (!JobsInitialized)
        return 0;

    EnterCriticalSection(&JobLock);

    for (Wanted = NextJobId - 1; Wanted > 0 && Count < Max; Wanted--)
    {
        PDRVINST_JOB_INFO Job = FindJob(Wanted);

        if (Job)
            Info[Count++] = *Job;
    }

    LeaveCriticalSection(&JobLock);

    return Count;
}

/* WORKER *******************************************************************/

static DWORD
QueryDrvInstValue(
    _In_ PCWSTR ValueName,
    _In_ DWORD Default)
{
    HKEY hKey;
    DWORD Value = Default, Size = sizeof(Value), Type;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, DRVINST_REG_KEY, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return Default;

    if (RegQueryValueExW(hKey, ValueName, NULL, &Type, (PBYTE)&Value, &Size) != ERROR_SUCCESS ||
        Type != REG_DWORD)
    {
        Value = Default;
    }

    RegCloseKey(hKey);
    return Value;
}

BOOL
DrvInstUseWorker(VOID)
{
    return JobsInitialized && QueryDrvInstValue(DRVINST_REG_USEWORKER, 0) != 0;
}

static DWORD
GetWorkerTimeout(VOID)
{
    DWORD Value = QueryDrvInstValue(DRVINST_REG_TIMEOUT, DRVINST_DEFAULT_TIMEOUT_MS);

    return Value ? Value : DRVINST_DEFAULT_TIMEOUT_MS;
}

/*
 * Start drvinst.exe for one device and wait for it, at most the time limit.
 * Fills Result; the return value is the state the job ends in.
 */
static ULONG
RunWorker(
    _In_ PCWSTR DeviceInstanceId,
    _In_ BOOL ShowWizard,
    _Out_ PDRVINST_RESULT Result)
{
    SECURITY_ATTRIBUTES Sa;
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    HANDLE hRead = NULL, hWrite = NULL;
    LPVOID Environment = NULL;
    WCHAR SystemDir[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + DRVINST_DEVICE_ID_CCH + 64];
    DWORD WaitResult, Read, ExitCode = 0;
    ULONG State = DrvInstJobFailed;
    BOOL Created = FALSE;

    ZeroMemory(Result, sizeof(*Result));
    Result->Size = sizeof(*Result);
    ZeroMemory(&ProcessInfo, sizeof(ProcessInfo));

    /* The worker inherits only the write end of the result pipe */
    Sa.nLength = sizeof(Sa);
    Sa.lpSecurityDescriptor = NULL;
    Sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hRead, &hWrite, &Sa, sizeof(*Result)))
    {
        Result->Win32 = GetLastError();
        return State;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    if (!GetWindowsDirectoryW(SystemDir, RTL_NUMBER_OF(SystemDir)))
    {
        Result->Win32 = GetLastError();
        goto Cleanup;
    }

    _snwprintf(CommandLine, RTL_NUMBER_OF(CommandLine), L"\"%ls\\system32\\%ls\" /i \"%ls\" /h %lx%ls",
               SystemDir, DRVINST_WORKER_NAME, DeviceInstanceId, (ULONG)(ULONG_PTR)hWrite,
               ShowWizard ? L" /w" : L"");
    CommandLine[RTL_NUMBER_OF(CommandLine) - 1] = UNICODE_NULL;

    ZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);

    if (hUserToken)
    {
        /* Like the installer before, run under the environment of the current user */
        if (!CreateEnvironmentBlock(&Environment, hUserToken, FALSE))
        {
            Result->Win32 = GetLastError();
            goto Cleanup;
        }

        Created = CreateProcessAsUserW(hUserToken, NULL, CommandLine, NULL, NULL, TRUE,
                                       CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                       Environment, NULL, &StartupInfo, &ProcessInfo);
    }
    else
    {
        Created = CreateProcessW(NULL, CommandLine, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                                 NULL, NULL, &StartupInfo, &ProcessInfo);
    }

    /* The service must not keep the write end open, or a read could never end */
    CloseHandle(hWrite);
    hWrite = NULL;

    if (!Created)
    {
        Result->Win32 = GetLastError();
        DPRINT1("Cannot start %ls for %ls (error %lu)\n", DRVINST_WORKER_NAME, DeviceInstanceId, Result->Win32);
        goto Cleanup;
    }

    WaitResult = WaitForSingleObject(ProcessInfo.hProcess, GetWorkerTimeout());
    if (WaitResult == WAIT_TIMEOUT)
    {
        DPRINT1("Install of %ls took too long, stopping the worker\n", DeviceInstanceId);
        TerminateProcess(ProcessInfo.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(ProcessInfo.hProcess, 5000);
        Result->Win32 = ERROR_TIMEOUT;
        State = DrvInstJobTimedOut;
        goto Cleanup;
    }

    GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);

    if (ReadFile(hRead, Result, sizeof(*Result), &Read, NULL) && Read == sizeof(*Result) &&
        Result->Size == sizeof(*Result))
    {
        State = (Result->Win32 == ERROR_SUCCESS) ? DrvInstJobSucceeded : DrvInstJobFailed;
    }
    else
    {
        /* The worker ended without reporting: it crashed or was stopped */
        ZeroMemory(Result, sizeof(*Result));
        Result->Size = sizeof(*Result);
        Result->Win32 = ExitCode ? ExitCode : ERROR_PROCESS_ABORTED;
        DPRINT1("%ls gave no result for %ls (exit code 0x%lx)\n", DRVINST_WORKER_NAME, DeviceInstanceId, ExitCode);
    }

Cleanup:
    if (Environment)
        DestroyEnvironmentBlock(Environment);
    if (ProcessInfo.hThread)
        CloseHandle(ProcessInfo.hThread);
    if (ProcessInfo.hProcess)
        CloseHandle(ProcessInfo.hProcess);
    if (hWrite)
        CloseHandle(hWrite);
    if (hRead)
        CloseHandle(hRead);

    return State;
}

/* Run a created job to its end in the calling thread. Installs take turns:
 * setupapi and the class installers are not meant to run twice at once. */
static BOOL
ExecuteJob(
    _In_ DRVINST_JOB_ID JobId,
    _In_ PCWSTR DeviceInstanceId,
    _In_ ULONG Flags)
{
    DRVINST_RESULT Result;
    ULONG State;

    WaitForSingleObject(hRunMutex, INFINITE);

    SetJobState(JobId, DrvInstJobRunning, NULL);
    State = RunWorker(DeviceInstanceId, !(Flags & DRVINST_F_SILENT), &Result);
    SetJobState(JobId, State, &Result);

    ReleaseMutex(hRunMutex);

    DPRINT("Job %I64u for %ls ended: state %lu, error %lu, problem %lu\n",
           JobId, DeviceInstanceId, State, Result.Win32, Result.Problem);

    return State == DrvInstJobSucceeded;
}

/* Run an install job for a device and wait for it. Returns TRUE when the driver was installed. */
BOOL
DrvInstRunDeviceJob(
    _In_ PCWSTR DeviceInstanceId,
    _In_ ULONG Source,
    _In_ ULONG Flags,
    _Out_opt_ PDRVINST_JOB_ID OutJobId)
{
    DRVINST_JOB_ID JobId;

    if (OutJobId)
        *OutJobId = 0;

    if (!JobsInitialized)
        return FALSE;

    JobId = CreateJob(DeviceInstanceId, Source, Flags);
    if (OutJobId)
        *OutJobId = JobId;
    if (!JobId)
        return FALSE;

    return ExecuteJob(JobId, DeviceInstanceId, Flags);
}

typedef struct _ASYNC_JOB
{
    DRVINST_JOB_ID JobId;
    ULONG Flags;
    WCHAR DeviceInstanceId[DRVINST_DEVICE_ID_CCH];
} ASYNC_JOB, *PASYNC_JOB;

static DWORD WINAPI
AsyncJobThread(LPVOID Parameter)
{
    PASYNC_JOB Async = Parameter;

    ExecuteJob(Async->JobId, Async->DeviceInstanceId, Async->Flags);
    HeapFree(GetProcessHeap(), 0, Async);

    return 0;
}

/* Queue an install job and return at once; the caller follows it with its id. */
DWORD
DrvInstSubmitDeviceJob(
    _In_ PCWSTR DeviceInstanceId,
    _In_ ULONG Source,
    _In_ ULONG Flags,
    _Out_ PDRVINST_JOB_ID OutJobId)
{
    PASYNC_JOB Async;
    DRVINST_JOB_ID JobId;
    HANDLE hThread;
    DWORD Error;

    *OutJobId = 0;

    if (!JobsInitialized)
        return ERROR_SERVICE_NOT_ACTIVE;
    if (!DeviceInstanceId[0] || lstrlenW(DeviceInstanceId) >= DRVINST_DEVICE_ID_CCH)
        return ERROR_INVALID_PARAMETER;

    Async = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Async));
    if (!Async)
        return ERROR_NOT_ENOUGH_MEMORY;

    Async->JobId = JobId = CreateJob(DeviceInstanceId, Source, Flags);
    if (!JobId)
    {
        HeapFree(GetProcessHeap(), 0, Async);
        return ERROR_NO_SYSTEM_RESOURCES;
    }
    Async->Flags = Flags;
    lstrcpynW(Async->DeviceInstanceId, DeviceInstanceId, DRVINST_DEVICE_ID_CCH);

    hThread = CreateThread(NULL, 0, AsyncJobThread, Async, 0, NULL);
    if (!hThread)
    {
        DRVINST_RESULT FailedResult;

        Error = GetLastError();
        ZeroMemory(&FailedResult, sizeof(FailedResult));
        FailedResult.Size = sizeof(FailedResult);
        FailedResult.Win32 = Error ? Error : ERROR_GEN_FAILURE;
        SetJobState(JobId, DrvInstJobFailed, &FailedResult);
        HeapFree(GetProcessHeap(), 0, Async);
        return FailedResult.Win32;
    }

    *OutJobId = JobId;
    CloseHandle(hThread);

    return ERROR_SUCCESS;
}
