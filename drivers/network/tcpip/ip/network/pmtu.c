/*
 * PROJECT:     LiberNT TCP/IP protocol driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IPv4 path MTU discovery (RFC 1191)
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif193@gmail.com>
 */

#include "precomp.h"

#include <icmp.h>
#include <lwip_glue/lwip_glue.h>

#define PMTU_TAG        'uMPT'
#define PMTU_MINIMUM    68

typedef struct _PMTU_ENTRY
{
    LIST_ENTRY ListEntry;
    PIP_INTERFACE Interface;
    IPv4_RAW_ADDRESS Destination;
    ULONG Mtu;
    ULONGLONG Changed;
} PMTU_ENTRY, *PPMTU_ENTRY;

static const ULONG PmtuPlateaus[] = { 65535, 32000, 17914, 8166, 4352, 2002, 1492, 1006, 508, 296, 68 };

static LIST_ENTRY PmtuEntries;
static KSPIN_LOCK PmtuLock;

static ULONG PmtuPlateauBelow(ULONG Length)
{
    ULONG i;

    for (i = 0; i < ARRAYSIZE(PmtuPlateaus); i++)
    {
        if (PmtuPlateaus[i] < Length)
            return PmtuPlateaus[i];
    }
    return PMTU_MINIMUM;
}

static ULONG PmtuPlateauAbove(ULONG Mtu)
{
    ULONG i, Above = PmtuPlateaus[0];

    for (i = 0; i < ARRAYSIZE(PmtuPlateaus); i++)
    {
        if (PmtuPlateaus[i] > Mtu)
            Above = PmtuPlateaus[i];
    }
    return Above;
}

static PPMTU_ENTRY PmtuFind(PIP_INTERFACE Interface, IPv4_RAW_ADDRESS Destination)
{
    PLIST_ENTRY Entry;

    for (Entry = PmtuEntries.Flink; Entry != &PmtuEntries; Entry = Entry->Flink)
    {
        PPMTU_ENTRY Pmtu = CONTAINING_RECORD(Entry, PMTU_ENTRY, ListEntry);

        if (Pmtu->Interface == Interface && Pmtu->Destination == Destination)
            return Pmtu;
    }
    return NULL;
}

BOOLEAN PmtuLower(PIP_INTERFACE Interface, IPv4_RAW_ADDRESS Destination, ULONG Mtu)
{
    PPMTU_ENTRY Pmtu, Fresh;
    BOOLEAN Lowered = FALSE;
    KIRQL OldIrql;

    Mtu = max(Mtu, PMTU_MINIMUM);
    if (Mtu >= Interface->MTU)
        return FALSE;

    Fresh = ExAllocatePoolZero(NonPagedPool, sizeof(*Fresh), PMTU_TAG);

    TcpipAcquireSpinLock(&PmtuLock, &OldIrql);
    Pmtu = PmtuFind(Interface, Destination);
    if (!Pmtu && Fresh)
    {
        Pmtu = Fresh;
        Fresh = NULL;
        Pmtu->Interface = Interface;
        Pmtu->Destination = Destination;
        Pmtu->Mtu = Interface->MTU;
        InsertTailList(&PmtuEntries, &Pmtu->ListEntry);
    }
    if (Pmtu && Mtu < Pmtu->Mtu)
    {
        Pmtu->Mtu = Mtu;
        Pmtu->Changed = KeQueryInterruptTime();
        Lowered = TRUE;
    }
    TcpipReleaseSpinLock(&PmtuLock, OldIrql);

    if (Fresh)
        ExFreePoolWithTag(Fresh, PMTU_TAG);
    return Lowered;
}

ULONG PmtuQuery(PIP_INTERFACE Interface, IPv4_RAW_ADDRESS Destination, PBOOLEAN Raised)
{
    ULONGLONG Now = KeQueryInterruptTime();
    PPMTU_ENTRY Pmtu, Expired = NULL;
    ULONG Mtu = Interface->MTU;
    KIRQL OldIrql;

    if (Raised)
        *Raised = FALSE;

    TcpipAcquireSpinLock(&PmtuLock, &OldIrql);
    Pmtu = PmtuFind(Interface, Destination);
    if (Pmtu)
    {
        if (Now - Pmtu->Changed >= (ULONGLONG)Interface->PathMtuDiscoveryTimeout * 10000)
        {
            Pmtu->Mtu = min(PmtuPlateauAbove(Pmtu->Mtu), Interface->MTU);
            Pmtu->Changed = Now;
            if (Raised)
                *Raised = TRUE;
        }
        Mtu = min(Pmtu->Mtu, Interface->MTU);
        if (Pmtu->Mtu >= Interface->MTU)
        {
            RemoveEntryList(&Pmtu->ListEntry);
            Expired = Pmtu;
        }
    }
    TcpipReleaseSpinLock(&PmtuLock, OldIrql);

    if (Expired)
        ExFreePoolWithTag(Expired, PMTU_TAG);
    return Mtu;
}

ULONG PmtuForDestination(IPv4_RAW_ADDRESS Destination, ULONG Default)
{
    PNEIGHBOR_CACHE_ENTRY NCE;
    IP_ADDRESS Address;
    ULONG Mtu;

    AddrInitIPv4(&Address, Destination);
    NCE = RouteGetRouteToDestination(&Address);
    if (!NCE)
        return Default;
    Mtu = PmtuQuery(NCE->Interface, Destination, NULL);
    NBDereferenceNeighbor(NCE);
    return Mtu;
}

VOID PmtuFlushInterface(PIP_INTERFACE Interface)
{
    PLIST_ENTRY Entry, Next;
    LIST_ENTRY Free;
    KIRQL OldIrql;

    InitializeListHead(&Free);
    TcpipAcquireSpinLock(&PmtuLock, &OldIrql);
    for (Entry = PmtuEntries.Flink; Entry != &PmtuEntries; Entry = Next)
    {
        PPMTU_ENTRY Pmtu = CONTAINING_RECORD(Entry, PMTU_ENTRY, ListEntry);

        Next = Entry->Flink;
        if (Pmtu->Interface == Interface)
        {
            RemoveEntryList(&Pmtu->ListEntry);
            InsertTailList(&Free, &Pmtu->ListEntry);
        }
    }
    TcpipReleaseSpinLock(&PmtuLock, OldIrql);

    while (!IsListEmpty(&Free))
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(&Free), PMTU_ENTRY, ListEntry), PMTU_TAG);
}

VOID PmtuReceive(PIP_INTERFACE Interface, PIP_PACKET IPPacket)
{
    PUCHAR Icmp = IPPacket->Data;
    UINT DataSize = IPPacket->TotalSize - IPPacket->HeaderSize;
    PIPv4_HEADER Quoted;
    PUCHAR Transport;
    IP_ADDRESS Source;
    ULONG QuotedHeader, QuotedLength, Mtu;

    if (DataSize < sizeof(ICMP_HEADER) + sizeof(IPv4_HEADER) + 8)
        return;

    Quoted = (PIPv4_HEADER)(Icmp + sizeof(ICMP_HEADER));
    QuotedHeader = (Quoted->VerIHL & 0x0F) * 4;
    if ((Quoted->VerIHL >> 4) != 4 || QuotedHeader < sizeof(IPv4_HEADER) ||
        DataSize < sizeof(ICMP_HEADER) + QuotedHeader + 8 || Quoted->Protocol != IPPROTO_TCP ||
        !(WN2H(Quoted->FlagsFragOfs) & IPv4_DF_MASK))
        return;

    AddrInitIPv4(&Source, Quoted->SrcAddr);
    if (!IsInterfaceAddress(Interface, &Source))
        return;

    QuotedLength = WN2H(Quoted->TotalLength);
    Mtu = (Icmp[6] << 8) | Icmp[7];
    if (!Mtu)
        Mtu = PmtuPlateauBelow(QuotedLength);
    if (Mtu >= QuotedLength)
        return;

    Transport = (PUCHAR)Quoted + QuotedHeader;
    LibTCPPathMtuExceeded(Interface, Quoted->SrcAddr, Quoted->DstAddr,
                          (USHORT)((Transport[0] << 8) | Transport[1]),
                          (USHORT)((Transport[2] << 8) | Transport[3]),
                          ((ULONG)Transport[4] << 24) | ((ULONG)Transport[5] << 16) |
                          ((ULONG)Transport[6] << 8) | Transport[7],
                          max(Mtu, PMTU_MINIMUM));
}

VOID ICMPSendFragmentationNeeded(PIP_INTERFACE Interface, PIP_PACKET IPPacket, ULONG Mtu)
{
    UINT QuotedSize = IPPacket->HeaderSize + min(8, IPPacket->TotalSize - IPPacket->HeaderSize);
    UINT DataSize = sizeof(ICMP_HEADER) + QuotedSize;
    PNEIGHBOR_CACHE_ENTRY NCE;
    ADDRESS_FILE FakeAddrFile;
    IP_PACKET NewPacket;
    IP_ADDRESS Source;
    PUCHAR Message;

    NCE = RouteGetRouteToDestination(&IPPacket->SrcAddr);
    if (!NCE)
        return;

    Message = ExAllocatePoolZero(NonPagedPool, DataSize, PMTU_TAG);
    if (!Message)
    {
        NBDereferenceNeighbor(NCE);
        return;
    }
    Message[0] = ICMP_TYPE_DEST_UNREACH;
    Message[1] = ICMP_CODE_DU_FRAG_DF_SET;
    Message[6] = (UCHAR)(Mtu >> 8);
    Message[7] = (UCHAR)Mtu;
    RtlCopyMemory(Message + sizeof(ICMP_HEADER), IPPacket->Header, IPPacket->HeaderSize);
    RtlCopyMemory(Message + sizeof(ICMP_HEADER) + IPPacket->HeaderSize, IPPacket->Data,
                  QuotedSize - IPPacket->HeaderSize);
    ((PICMP_HEADER)Message)->Checksum = (USHORT)IPv4Checksum(Message, DataSize, 0);

    GetInterfaceSourceAddress(Interface, &IPPacket->SrcAddr, &NCE->Address, &Source);
    RtlZeroMemory(&FakeAddrFile, sizeof(FakeAddrFile));
    FakeAddrFile.Protocol = IPPROTO_ICMP;
    FakeAddrFile.TTL = 128;
    if (NT_SUCCESS(BuildRawIpPacket(&FakeAddrFile, &NewPacket, &IPPacket->SrcAddr, 0, &Source, 0, (PCHAR)Message, DataSize)))
        IPSendDatagram(&NewPacket, NCE);

    ExFreePoolWithTag(Message, PMTU_TAG);
    NBDereferenceNeighbor(NCE);
}

VOID PmtuStartup(VOID)
{
    InitializeListHead(&PmtuEntries);
    KeInitializeSpinLock(&PmtuLock);
}
