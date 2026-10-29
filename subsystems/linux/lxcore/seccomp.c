/*
 * PROJECT:     LiberNT Linux Subsystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Secure computing mode: strict mode and classic BPF system call filters
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "lxcore.h"

#define NDEBUG
#include <debug.h>

#define LX_SECCOMP_SET_MODE_STRICT          0
#define LX_SECCOMP_SET_MODE_FILTER          1
#define LX_SECCOMP_GET_ACTION_AVAIL         2

#define LX_SECCOMP_MODE_DISABLED            0
#define LX_SECCOMP_MODE_STRICT              1
#define LX_SECCOMP_MODE_FILTER              2

#define LX_SECCOMP_FILTER_FLAG_TSYNC        0x01
#define LX_SECCOMP_FILTER_FLAG_LOG          0x02
#define LX_SECCOMP_FILTER_FLAG_SPEC_ALLOW   0x04
#define LX_SECCOMP_FILTER_FLAG_TSYNC_ESRCH  0x10

#define LX_SECCOMP_RET_KILL_PROCESS         0x80000000UL
#define LX_SECCOMP_RET_KILL_THREAD          0x00000000UL
#define LX_SECCOMP_RET_TRAP                 0x00030000UL
#define LX_SECCOMP_RET_ERRNO                0x00050000UL
#define LX_SECCOMP_RET_USER_NOTIF           0x7FC00000UL
#define LX_SECCOMP_RET_TRACE                0x7FF00000UL
#define LX_SECCOMP_RET_LOG                  0x7FFC0000UL
#define LX_SECCOMP_RET_ALLOW                0x7FFF0000UL
#define LX_SECCOMP_RET_ACTION_FULL          0xFFFF0000UL
#define LX_SECCOMP_RET_DATA                 0x0000FFFFUL

#define LX_SYS_SECCOMP                      1
#define LX_AUDIT_ARCH_64BIT                 0x80000000UL
#define LX_AUDIT_ARCH_LE                    0x40000000UL
#define LX_MAX_ERRNO                        4095

#define LX_CBPF_MAXINSNS                    4096
#define LX_CBPF_MEMWORDS                    16
#define LX_SECCOMP_MAX_INSNS_PER_PATH       32768
#define LX_SECCOMP_DATA_SIZE                64

#define LX_CBPF_LD                          0x00
#define LX_CBPF_LDX                         0x01
#define LX_CBPF_ST                          0x02
#define LX_CBPF_STX                         0x03
#define LX_CBPF_ALU                         0x04
#define LX_CBPF_JMP                         0x05
#define LX_CBPF_RET                         0x06
#define LX_CBPF_MISC                        0x07

#define LX_CBPF_LD_IMM                      0x00
#define LX_CBPF_LD_ABS                      0x20
#define LX_CBPF_LD_MEM                      0x60
#define LX_CBPF_LD_LEN                      0x80
#define LX_CBPF_LDX_IMM                     0x01
#define LX_CBPF_LDX_MEM                     0x61
#define LX_CBPF_LDX_LEN                     0x81
#define LX_CBPF_RET_K                       0x06
#define LX_CBPF_RET_A                       0x16
#define LX_CBPF_TAX                         0x07
#define LX_CBPF_TXA                         0x87

#define LX_CBPF_SRC_X                       0x08
#define LX_CBPF_ADD                         0x00
#define LX_CBPF_SUB                         0x10
#define LX_CBPF_MUL                         0x20
#define LX_CBPF_DIV                         0x30
#define LX_CBPF_OR                          0x40
#define LX_CBPF_AND                         0x50
#define LX_CBPF_LSH                         0x60
#define LX_CBPF_RSH                         0x70
#define LX_CBPF_NEG                         0x80
#define LX_CBPF_MOD                         0x90
#define LX_CBPF_XOR                         0xA0
#define LX_CBPF_JA                          0x00
#define LX_CBPF_JEQ                         0x10
#define LX_CBPF_JGT                         0x20
#define LX_CBPF_JGE                         0x30
#define LX_CBPF_JSET                        0x40

typedef struct _LX_SOCK_FILTER
{
    USHORT Code;
    UCHAR Jt;
    UCHAR Jf;
    ULONG K;
} LX_SOCK_FILTER, *PLX_SOCK_FILTER;

C_ASSERT(sizeof(LX_SOCK_FILTER) == 8);

typedef struct _LX_SOCK_FPROG
{
    USHORT Length;
    USHORT Pad[3];
    ULONG64 Filter;
} LX_SOCK_FPROG;

C_ASSERT(sizeof(LX_SOCK_FPROG) == 16);

struct _LX_SECCOMP_FILTER
{
    volatile LONG References;
    PLX_SECCOMP_FILTER Previous;
    ULONG Length;
    ULONG PathLength;
    BOOLEAN Log;
    LX_SOCK_FILTER Insns[ANYSIZE_ARRAY];
};

static
VOID
LxSeccompReference(
    _In_ PLX_SECCOMP_FILTER Filter)
{
    InterlockedIncrement(&Filter->References);
}

static
VOID
LxSeccompDereference(
    _In_opt_ PLX_SECCOMP_FILTER Filter)
{
    while ((Filter != NULL) && (InterlockedDecrement(&Filter->References) == 0))
    {
        PLX_SECCOMP_FILTER Previous = Filter->Previous;

        LxFree(Filter);
        Filter = Previous;
    }
}

VOID
LxSeccompInherit(
    _Inout_ PLX_THREAD Thread,
    _In_ PLX_THREAD Parent)
{
    PLX_SECCOMP_FILTER Filter = (PLX_SECCOMP_FILTER)ReadPointerAcquire((PVOID volatile *)&Parent->Seccomp);

    if (Filter != NULL)
        LxSeccompReference(Filter);
    Thread->Seccomp = Filter;
    Thread->SeccompMode = Parent->SeccompMode;
}

VOID
LxSeccompRelease(
    _Inout_ PLX_THREAD Thread)
{
    PLX_SECCOMP_FILTER Filter = (PLX_SECCOMP_FILTER)InterlockedExchangePointer((PVOID volatile *)&Thread->Seccomp, NULL);

    LxSeccompDereference(Filter);
}

ULONG
LxSeccompFilterCount(
    _In_ PLX_THREAD Thread)
{
    PLX_SECCOMP_FILTER Filter = (PLX_SECCOMP_FILTER)ReadPointerAcquire((PVOID volatile *)&Thread->Seccomp);
    ULONG Count = 0;

    for (; Filter != NULL; Filter = Filter->Previous)
        Count++;
    return Count;
}

static
LONG
LxSeccompValidate(
    _Inout_updates_(Length) PLX_SOCK_FILTER Insns,
    _In_ ULONG Length)
{
    ULONG Pc;

    for (Pc = 0; Pc < Length; Pc++)
    {
        PLX_SOCK_FILTER Insn = &Insns[Pc];
        ULONG Remaining = Length - Pc - 1;

        if (Insn->Code > 0xFF)
            return -LX_EINVAL;
        switch (Insn->Code & 0x07)
        {
            case LX_CBPF_LD:
                if (Insn->Code == LX_CBPF_LD_ABS)
                {
                    if ((Insn->K >= LX_SECCOMP_DATA_SIZE) || (Insn->K & 3))
                        return -LX_EINVAL;
                }
                else if (Insn->Code == LX_CBPF_LD_LEN)
                {
                    Insn->Code = LX_CBPF_LD_IMM;
                    Insn->K = LX_SECCOMP_DATA_SIZE;
                }
                else if (Insn->Code == LX_CBPF_LD_MEM)
                {
                    if (Insn->K >= LX_CBPF_MEMWORDS)
                        return -LX_EINVAL;
                }
                else if (Insn->Code != LX_CBPF_LD_IMM)
                {
                    return -LX_EINVAL;
                }
                break;

            case LX_CBPF_LDX:
                if (Insn->Code == LX_CBPF_LDX_LEN)
                {
                    Insn->Code = LX_CBPF_LDX_IMM;
                    Insn->K = LX_SECCOMP_DATA_SIZE;
                }
                else if (Insn->Code == LX_CBPF_LDX_MEM)
                {
                    if (Insn->K >= LX_CBPF_MEMWORDS)
                        return -LX_EINVAL;
                }
                else if (Insn->Code != LX_CBPF_LDX_IMM)
                {
                    return -LX_EINVAL;
                }
                break;

            case LX_CBPF_ST:
            case LX_CBPF_STX:
                if ((Insn->Code != LX_CBPF_ST) && (Insn->Code != LX_CBPF_STX))
                    return -LX_EINVAL;
                if (Insn->K >= LX_CBPF_MEMWORDS)
                    return -LX_EINVAL;
                break;

            case LX_CBPF_ALU:
                switch (Insn->Code & 0xF0)
                {
                    case LX_CBPF_ADD:
                    case LX_CBPF_SUB:
                    case LX_CBPF_MUL:
                    case LX_CBPF_OR:
                    case LX_CBPF_AND:
                    case LX_CBPF_XOR:
                        break;
                    case LX_CBPF_DIV:
                    case LX_CBPF_MOD:
                        if (!(Insn->Code & LX_CBPF_SRC_X) && (Insn->K == 0))
                            return -LX_EINVAL;
                        break;
                    case LX_CBPF_LSH:
                    case LX_CBPF_RSH:
                        if (!(Insn->Code & LX_CBPF_SRC_X) && (Insn->K >= 32))
                            return -LX_EINVAL;
                        break;
                    case LX_CBPF_NEG:
                        if (Insn->Code & LX_CBPF_SRC_X)
                            return -LX_EINVAL;
                        break;
                    default:
                        return -LX_EINVAL;
                }
                break;

            case LX_CBPF_JMP:
                switch (Insn->Code & 0xF0)
                {
                    case LX_CBPF_JA:
                        if ((Insn->Code & LX_CBPF_SRC_X) || (Insn->K >= Remaining))
                            return -LX_EINVAL;
                        break;
                    case LX_CBPF_JEQ:
                    case LX_CBPF_JGT:
                    case LX_CBPF_JGE:
                    case LX_CBPF_JSET:
                        if ((Insn->Jt >= Remaining) || (Insn->Jf >= Remaining))
                            return -LX_EINVAL;
                        break;
                    default:
                        return -LX_EINVAL;
                }
                break;

            case LX_CBPF_RET:
                if ((Insn->Code != LX_CBPF_RET_K) && (Insn->Code != LX_CBPF_RET_A))
                    return -LX_EINVAL;
                break;

            case LX_CBPF_MISC:
                if ((Insn->Code != LX_CBPF_TAX) && (Insn->Code != LX_CBPF_TXA))
                    return -LX_EINVAL;
                break;
        }
    }
    if ((Length == 0) || ((Insns[Length - 1].Code & 0x07) != LX_CBPF_RET))
        return -LX_EINVAL;
    return 0;
}

static
ULONG
LxSeccompRun(
    _In_ PLX_SECCOMP_FILTER Filter,
    _In_reads_(LX_SECCOMP_DATA_SIZE / sizeof(ULONG)) const ULONG *Data)
{
    ULONG A = 0, X = 0, Pc = 0, Source;
    ULONG Memory[LX_CBPF_MEMWORDS];
    BOOLEAN Taken;

    RtlZeroMemory(Memory, sizeof(Memory));
    while (Pc < Filter->Length)
    {
        const LX_SOCK_FILTER *Insn = &Filter->Insns[Pc++];

        switch (Insn->Code & 0x07)
        {
            case LX_CBPF_LD:
                if (Insn->Code == LX_CBPF_LD_ABS)
                    A = Data[Insn->K / sizeof(ULONG)];
                else if (Insn->Code == LX_CBPF_LD_MEM)
                    A = Memory[Insn->K];
                else
                    A = Insn->K;
                break;

            case LX_CBPF_LDX:
                X = (Insn->Code == LX_CBPF_LDX_MEM) ? Memory[Insn->K] : Insn->K;
                break;

            case LX_CBPF_ST:
                Memory[Insn->K] = A;
                break;

            case LX_CBPF_STX:
                Memory[Insn->K] = X;
                break;

            case LX_CBPF_ALU:
                Source = (Insn->Code & LX_CBPF_SRC_X) ? X : Insn->K;
                switch (Insn->Code & 0xF0)
                {
                    case LX_CBPF_ADD: A += Source; break;
                    case LX_CBPF_SUB: A -= Source; break;
                    case LX_CBPF_MUL: A *= Source; break;
                    case LX_CBPF_OR: A |= Source; break;
                    case LX_CBPF_AND: A &= Source; break;
                    case LX_CBPF_XOR: A ^= Source; break;
                    case LX_CBPF_LSH: A <<= (Source & 31); break;
                    case LX_CBPF_RSH: A >>= (Source & 31); break;
                    case LX_CBPF_NEG: A = 0 - A; break;
                    case LX_CBPF_DIV:
                        if (Source == 0)
                            return 0;
                        A /= Source;
                        break;
                    case LX_CBPF_MOD:
                        if (Source == 0)
                            return 0;
                        A %= Source;
                        break;
                }
                break;

            case LX_CBPF_JMP:
                if ((Insn->Code & 0xF0) == LX_CBPF_JA)
                {
                    Pc += Insn->K;
                    break;
                }
                Source = (Insn->Code & LX_CBPF_SRC_X) ? X : Insn->K;
                switch (Insn->Code & 0xF0)
                {
                    case LX_CBPF_JEQ: Taken = (A == Source); break;
                    case LX_CBPF_JGT: Taken = (A > Source); break;
                    case LX_CBPF_JGE: Taken = (A >= Source); break;
                    default: Taken = ((A & Source) != 0); break;
                }
                Pc += Taken ? Insn->Jt : Insn->Jf;
                break;

            case LX_CBPF_RET:
                return (Insn->Code == LX_CBPF_RET_A) ? A : Insn->K;

            case LX_CBPF_MISC:
                if (Insn->Code == LX_CBPF_TAX)
                    X = A;
                else
                    A = X;
                break;
        }
    }
    return 0;
}

static
ULONG
LxSeccompAuditArch(VOID)
{
    return LxArchElfMachine() | LX_AUDIT_ARCH_LE | ((sizeof(PVOID) == 8) ? LX_AUDIT_ARCH_64BIT : 0);
}

static
VOID
LxSeccompKill(
    _Inout_ PLX_CALL Call,
    _In_ BOOLEAN WholeProcess)
{
    LONG Status = LX_SIGSYS;

    if (!WholeProcess && (Call->Process->ThreadCount > 1))
    {
        PsPicoTerminateCurrentThread((NTSTATUS)LX_SIGSYS);
        return;
    }
    if (LxCoreDump(Call->Thread, Call->TrapFrame, Call->ExceptionFrame, LX_SIGSYS))
        Status |= 0x80;
    LxProcessExitGroup(Call->Process, Status);
}

BOOLEAN
LxSeccompCheck(
    _Inout_ PLX_CALL Call,
    _Out_ PLONG64 Result)
{
    PLX_THREAD Thread = Call->Thread;
    PLX_SECCOMP_FILTER Filter, Current;
    ULONG Data[LX_SECCOMP_DATA_SIZE / sizeof(ULONG)];
    ULONG64 Pc = LxArchGetUserPc(Call->TrapFrame);
    ULONG Action = LX_SECCOMP_RET_ALLOW, Verdict, Index;
    LX_SIGINFO Info;

    *Result = LX_SYSCALL_NO_RESULT;
    if (Thread->SeccompMode == LX_SECCOMP_MODE_STRICT)
    {
        PLX_SYSCALL_ENTRY Entry = LxArchSyscallEntry(Call->Number);

        if ((Entry != NULL) && ((Entry->Handler == LxSysRead) || (Entry->Handler == LxSysWrite) ||
                                (Entry->Handler == LxSysExit) || (Entry->Handler == LxSysRtSigreturn)))
        {
            return TRUE;
        }
        LxSeccompKill(Call, TRUE);
        return FALSE;
    }

    Filter = (PLX_SECCOMP_FILTER)ReadPointerAcquire((PVOID volatile *)&Thread->Seccomp);
    if (Filter == NULL)
        return TRUE;

    Data[0] = Call->Number;
    Data[1] = LxSeccompAuditArch();
    Data[2] = (ULONG)Pc;
    Data[3] = (ULONG)(Pc >> 32);
    for (Index = 0; Index < 6; Index++)
    {
        Data[4 + Index * 2] = (ULONG)Call->Args[Index];
        Data[5 + Index * 2] = (ULONG)(Call->Args[Index] >> 32);
    }

    for (Current = Filter; Current != NULL; Current = Current->Previous)
    {
        Verdict = LxSeccompRun(Current, Data);
        if ((LONG)(Verdict & LX_SECCOMP_RET_ACTION_FULL) < (LONG)(Action & LX_SECCOMP_RET_ACTION_FULL))
            Action = Verdict;
    }

    switch (Action & LX_SECCOMP_RET_ACTION_FULL)
    {
        /* TODO: log SECCOMP_RET_LOG actions and filters installed with SECCOMP_FILTER_FLAG_LOG; nothing is recorded yet. */
        case LX_SECCOMP_RET_ALLOW:
        case LX_SECCOMP_RET_LOG:
            return TRUE;

        case LX_SECCOMP_RET_ERRNO:
            *Result = -(LONG64)min(Action & LX_SECCOMP_RET_DATA, LX_MAX_ERRNO);
            return FALSE;

        /* TODO: stop for a ptrace tracer on SECCOMP_RET_TRACE once ptrace exists; ENOSYS matches Linux without a tracer. */
        /* TODO: deliver SECCOMP_RET_USER_NOTIF to a listener; ENOSYS matches Linux without one. */
        case LX_SECCOMP_RET_TRACE:
        case LX_SECCOMP_RET_USER_NOTIF:
            *Result = -LX_ENOSYS;
            return FALSE;

        case LX_SECCOMP_RET_TRAP:
            RtlZeroMemory(&Info, sizeof(Info));
            Info.Signo = LX_SIGSYS;
            Info.Errno = (LONG)(Action & LX_SECCOMP_RET_DATA);
            Info.Code = LX_SYS_SECCOMP;
            RtlCopyMemory(&Info.Raw[0], &Pc, sizeof(Pc));
            RtlCopyMemory(&Info.Raw[8], &Data[0], sizeof(ULONG));
            RtlCopyMemory(&Info.Raw[12], &Data[1], sizeof(ULONG));
            if (!LxSignalDeliverFault(Thread, Call->TrapFrame, Call->ExceptionFrame, LX_SIGSYS, &Info))
                LxSeccompKill(Call, TRUE);
            return FALSE;

        case LX_SECCOMP_RET_KILL_THREAD:
            LxSeccompKill(Call, FALSE);
            return FALSE;

        default:
            LxSeccompKill(Call, TRUE);
            return FALSE;
    }
}

static
BOOLEAN
LxSeccompIsAncestor(
    _In_opt_ PLX_SECCOMP_FILTER Candidate,
    _In_opt_ PLX_SECCOMP_FILTER Chain)
{
    if (Candidate == NULL)
        return TRUE;
    for (; Chain != NULL; Chain = Chain->Previous)
    {
        if (Chain == Candidate)
            return TRUE;
    }
    return FALSE;
}

static
LONG64
LxSeccompSetStrict(
    _Inout_ PLX_CALL Call)
{
    PLX_THREAD Thread = Call->Thread;

    if (Thread->SeccompMode == LX_SECCOMP_MODE_FILTER)
        return -LX_EINVAL;
    Thread->SeccompMode = LX_SECCOMP_MODE_STRICT;
    return 0;
}

static
LONG64
LxSeccompSetFilter(
    _Inout_ PLX_CALL Call,
    _In_ ULONG Flags,
    _In_ ULONG64 UserProgram)
{
    PLX_PROCESS Process = Call->Process;
    PLX_THREAD Thread = Call->Thread;
    PLX_SECCOMP_FILTER Filter, Previous;
    LX_SOCK_FPROG Program;
    PLIST_ENTRY Entry;
    LONG64 Result;
    SIZE_T Size;

    /* TODO: SECCOMP_FILTER_FLAG_SPEC_ALLOW is accepted but no speculative store bypass mitigation is applied either way. */
    /* TODO: accept SECCOMP_FILTER_FLAG_NEW_LISTENER and SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV with user notifications. */
    if (Flags & ~(LX_SECCOMP_FILTER_FLAG_TSYNC | LX_SECCOMP_FILTER_FLAG_LOG | LX_SECCOMP_FILTER_FLAG_SPEC_ALLOW |
                  LX_SECCOMP_FILTER_FLAG_TSYNC_ESRCH))
    {
        return -LX_EINVAL;
    }

    Result = LxCopyFromUser(&Program, UserProgram, sizeof(Program));
    if (Result != 0)
        return Result;
    if ((Program.Length == 0) || (Program.Length > LX_CBPF_MAXINSNS))
        return -LX_EINVAL;
    if (!Process->NoNewPrivs && !LxCapable(Process, LX_CAP_SYS_ADMIN))
        return -LX_EACCES;

    Size = FIELD_OFFSET(LX_SECCOMP_FILTER, Insns) + (SIZE_T)Program.Length * sizeof(LX_SOCK_FILTER);
    Filter = LxAlloc(Size);
    if (Filter == NULL)
        return -LX_ENOMEM;
    Filter->References = 1;
    Filter->Length = Program.Length;
    Filter->Log = (Flags & LX_SECCOMP_FILTER_FLAG_LOG) != 0;
    Result = LxCopyFromUser(Filter->Insns, Program.Filter, (SIZE_T)Program.Length * sizeof(LX_SOCK_FILTER));
    if (Result == 0)
        Result = LxSeccompValidate(Filter->Insns, Filter->Length);
    if (Result != 0)
    {
        LxFree(Filter);
        return Result;
    }

    ExAcquireFastMutex(&Process->Lock);
    Previous = Thread->Seccomp;
    Filter->PathLength = Filter->Length + ((Previous != NULL) ? Previous->PathLength + 4 : 0);
    if (Thread->SeccompMode == LX_SECCOMP_MODE_STRICT)
        Result = -LX_EINVAL;
    else if (Filter->PathLength > LX_SECCOMP_MAX_INSNS_PER_PATH)
        Result = -LX_ENOMEM;

    if ((Result == 0) && (Flags & LX_SECCOMP_FILTER_FLAG_TSYNC))
    {
        for (Entry = Process->Threads.Flink; Entry != &Process->Threads; Entry = Entry->Flink)
        {
            PLX_THREAD Other = CONTAINING_RECORD(Entry, LX_THREAD, Link);

            if (Other == Thread)
                continue;
            if ((Other->SeccompMode == LX_SECCOMP_MODE_STRICT) || !LxSeccompIsAncestor(Other->Seccomp, Previous))
            {
                Result = (Flags & LX_SECCOMP_FILTER_FLAG_TSYNC_ESRCH) ? -LX_ESRCH : (LONG64)LxTidNr(Process, Other);
                break;
            }
        }
    }

    if (Result != 0)
    {
        ExReleaseFastMutex(&Process->Lock);
        LxFree(Filter);
        return Result;
    }

    Filter->Previous = Previous;
    InterlockedExchangePointer((PVOID volatile *)&Thread->Seccomp, Filter);
    Thread->SeccompMode = LX_SECCOMP_MODE_FILTER;

    if (Flags & LX_SECCOMP_FILTER_FLAG_TSYNC)
    {
        for (Entry = Process->Threads.Flink; Entry != &Process->Threads; Entry = Entry->Flink)
        {
            PLX_THREAD Other = CONTAINING_RECORD(Entry, LX_THREAD, Link);
            PLX_SECCOMP_FILTER Old;

            if (Other == Thread)
                continue;
            LxSeccompReference(Filter);
            Old = (PLX_SECCOMP_FILTER)InterlockedExchangePointer((PVOID volatile *)&Other->Seccomp, Filter);
            Other->SeccompMode = LX_SECCOMP_MODE_FILTER;
            LxSeccompDereference(Old);
        }
    }
    ExReleaseFastMutex(&Process->Lock);
    return 0;
}

/* TODO: expose /proc/sys/kernel/seccomp/actions_avail and actions_logged. */
static
LONG64
LxSeccompActionAvailable(
    _In_ ULONG64 UserAction)
{
    ULONG Action;
    LONG64 Result = LxCopyFromUser(&Action, UserAction, sizeof(Action));

    if (Result != 0)
        return Result;
    switch (Action)
    {
        case LX_SECCOMP_RET_KILL_PROCESS:
        case LX_SECCOMP_RET_KILL_THREAD:
        case LX_SECCOMP_RET_TRAP:
        case LX_SECCOMP_RET_ERRNO:
        case LX_SECCOMP_RET_TRACE:
        case LX_SECCOMP_RET_LOG:
        case LX_SECCOMP_RET_ALLOW:
            return 0;
        default:
            return -LX_EOPNOTSUPP;
    }
}

LONG64
LxSeccompPrctl(
    _Inout_ PLX_CALL Call,
    _In_ BOOLEAN Set,
    _In_ ULONG64 Mode,
    _In_ ULONG64 Program)
{
    if (!Set)
        return Call->Thread->SeccompMode;
    if (Mode == LX_SECCOMP_MODE_STRICT)
        return LxSeccompSetStrict(Call);
    if (Mode == LX_SECCOMP_MODE_FILTER)
        return LxSeccompSetFilter(Call, 0, Program);
    return -LX_EINVAL;
}

LX_SYSCALL(LxSysSeccomp)
{
    ULONG Operation = (ULONG)Call->Args[0];
    ULONG Flags = (ULONG)Call->Args[1];

    switch (Operation)
    {
        case LX_SECCOMP_SET_MODE_STRICT:
            if ((Flags != 0) || (Call->Args[2] != 0))
                return -LX_EINVAL;
            return LxSeccompSetStrict(Call);

        case LX_SECCOMP_SET_MODE_FILTER:
            return LxSeccompSetFilter(Call, Flags, Call->Args[2]);

        case LX_SECCOMP_GET_ACTION_AVAIL:
            if (Flags != 0)
                return -LX_EINVAL;
            return LxSeccompActionAvailable(Call->Args[2]);

        /* TODO: SECCOMP_GET_NOTIF_SIZES needs user notifications. */
        default:
            return -LX_EINVAL;
    }
}
