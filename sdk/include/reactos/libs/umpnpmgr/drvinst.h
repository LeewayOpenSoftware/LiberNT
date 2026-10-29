/*
 * PROJECT:     LiberNT Plug and Play Manager
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver install jobs shared by the service, its worker and its clients
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#ifndef _UMPNPMGR_DRVINST_H_
#define _UMPNPMGR_DRVINST_H_

/* A driver install job is one attempt to install the driver of one device. */
typedef ULONG64 DRVINST_JOB_ID;
typedef DRVINST_JOB_ID *PDRVINST_JOB_ID;

typedef enum _DRVINST_JOB_STATE
{
    DrvInstJobQueued = 0,
    DrvInstJobRunning,
    DrvInstJobSucceeded,
    DrvInstJobFailed,
    DrvInstJobTimedOut,
    DrvInstJobCanceled
} DRVINST_JOB_STATE;

/* Where the job came from */
#define DRVINST_SOURCE_ARRIVAL      1   /* the device was enumerated */
#define DRVINST_SOURCE_USER         2   /* a client asked for it */
#define DRVINST_SOURCE_SETUP        3   /* the installer of the system */

/* Job flags */
#define DRVINST_F_SILENT            0x00000001  /* never show the wizard */

/* DRVINST_RESULT.Flags */
#define DRVINST_RF_STARTED          0x00000001  /* the devnode was started when the job ended */
#define DRVINST_RF_NEEDREBOOT       0x00000002  /* the install asked for a restart */

/*
 * What the worker reports back. Win32 is the real error of the install
 * (0 on success); Problem is the CM_PROB_* code of the devnode afterwards
 * (0 when it has none) and Cr is the result of reading it.
 */
typedef struct _DRVINST_RESULT
{
    ULONG Size;
    ULONG Win32;
    ULONG Cr;
    ULONG DevStatus;
    ULONG Problem;
    ULONG Flags;
} DRVINST_RESULT, *PDRVINST_RESULT;

#define DRVINST_DEVICE_ID_CCH       200

/* One job as the service shows it to a client */
typedef struct _DRVINST_JOB_INFO
{
    ULONG Size;
    DRVINST_JOB_ID JobId;
    ULONG State;                        /* DRVINST_JOB_STATE */
    ULONG Source;
    ULONG Flags;
    ULONG SubmitTick;                   /* GetTickCount() at submission */
    ULONG EndTick;                      /* 0 while the job has not ended */
    DRVINST_RESULT Result;              /* valid once the job has ended */
    WCHAR DeviceInstanceId[DRVINST_DEVICE_ID_CCH];
} DRVINST_JOB_INFO, *PDRVINST_JOB_INFO;

/* Worker (drvinst.exe) command line:
 *   drvinst.exe /i "<device instance id>" /h <result pipe handle in hex> [/w]
 * /w lets the install show its wizard. The worker writes one DRVINST_RESULT
 * to the inherited handle and exits with the same Win32 value. */
#define DRVINST_WORKER_NAME         L"drvinst.exe"

/* Registry: HKLM\SYSTEM\CurrentControlSet\Control\DrvInst
 *   UseWorker (REG_DWORD)  1 = the service installs through the worker
 *   WorkerTimeoutMs (REG_DWORD)  how long one install may run (default 600000) */
#define DRVINST_REG_KEY             L"SYSTEM\\CurrentControlSet\\Control\\DrvInst"
#define DRVINST_REG_USEWORKER       L"UseWorker"
#define DRVINST_REG_TIMEOUT         L"WorkerTimeoutMs"
#define DRVINST_DEFAULT_TIMEOUT_MS  600000

/* Client API, exported by newdev.dll. All return a Win32 error (0 = success). */
DWORD WINAPI DrvInstSubmitDevice(_In_ PCWSTR DeviceInstanceId, _In_ DWORD Flags, _Out_ DRVINST_JOB_ID *JobId);
DWORD WINAPI DrvInstGetJob(_In_ DRVINST_JOB_ID JobId, _Out_ PDRVINST_JOB_INFO Info);
DWORD WINAPI DrvInstEnumJobs(_Out_writes_to_(*Count, *Count) DRVINST_JOB_ID *JobIds, _Inout_ PDWORD Count);
/* Waits until the job has ended or TimeoutMs has passed (ERROR_TIMEOUT). Info may be NULL. */
DWORD WINAPI DrvInstWait(_In_ DRVINST_JOB_ID JobId, _In_ DWORD TimeoutMs, _Out_opt_ PDRVINST_JOB_INFO Info);

#endif /* _UMPNPMGR_DRVINST_H_ */
