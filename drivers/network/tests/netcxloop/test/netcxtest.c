/*
 * PROJECT:     LiberNT network tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Installs the NetAdapterCx test adapter and pings its simulated link peer
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif193.001@gmail.com>
 */

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <netioapi.h>
#include <stdarg.h>
#include <stdio.h>

static const UCHAR AdapterAddress[6] = { 0x02, 0x00, 0x4C, 0x4F, 0x4F, 0x50 };

static int Failures;

static
void
Report(const char *Format, ...)
{
    char Buffer[512];
    va_list Args;

    va_start(Args, Format);
    _vsnprintf(Buffer, sizeof(Buffer) - 2, Format, Args);
    va_end(Args);
    Buffer[sizeof(Buffer) - 2] = 0;
    strcat(Buffer, "\n");
    fputs(Buffer, stdout);
    OutputDebugStringA(Buffer);
}

static
void
Check(BOOL Condition, const char *What)
{
    if (!Condition)
        Failures++;
    Report("NETCXTEST: %s %s", Condition ? "ok" : "FAIL", What);
}

static
BOOL
InstallDevice(const WCHAR *InfPath)
{
    static const WCHAR HardwareId[] = L"ROOT\\NETCXLOOP\0";
    WCHAR ClassName[MAX_CLASS_NAME_LEN];
    SP_DEVINFO_DATA Info;
    HDEVINFO Set;
    GUID ClassGuid;
    BOOL Reboot = FALSE;
    BOOL Result;

    if (!SetupDiGetINFClassW(InfPath, &ClassGuid, ClassName, ARRAYSIZE(ClassName), NULL))
    {
        Report("NETCXTEST: SetupDiGetINFClass error %lu", GetLastError());
        return FALSE;
    }

    Set = SetupDiCreateDeviceInfoList(&ClassGuid, NULL);
    if (Set == INVALID_HANDLE_VALUE)
    {
        Report("NETCXTEST: SetupDiCreateDeviceInfoList error %lu", GetLastError());
        return FALSE;
    }

    Info.cbSize = sizeof(Info);
    Result = SetupDiCreateDeviceInfoW(Set, ClassName, &ClassGuid, NULL, NULL, DICD_GENERATE_ID, &Info) &&
             SetupDiSetDeviceRegistryPropertyW(Set, &Info, SPDRP_HARDWAREID, (const BYTE *)HardwareId, sizeof(HardwareId)) &&
             SetupDiCallClassInstaller(DIF_REGISTERDEVICE, Set, &Info);
    if (!Result)
    {
        Report("NETCXTEST: device registration error %lu", GetLastError());
        SetupDiDestroyDeviceInfoList(Set);
        return FALSE;
    }

    Result = UpdateDriverForPlugAndPlayDevicesW(NULL, L"ROOT\\NETCXLOOP", InfPath, INSTALLFLAG_FORCE, &Reboot);
    if (!Result)
        Report("NETCXTEST: UpdateDriverForPlugAndPlayDevices error %lu", GetLastError());
    else
        Report("NETCXTEST: driver installed, reboot %d", Reboot);
    SetupDiDestroyDeviceInfoList(Set);
    return Result;
}

static
BOOL
FindInterface(MIB_IF_ROW2 *Row)
{
    MIB_IF_TABLE2 *Table;
    BOOL Found = FALSE;
    ULONG i;

    if (GetIfTable2(&Table) != NO_ERROR)
        return FALSE;
    for (i = 0; i < Table->NumEntries; i++)
    {
        if (Table->Table[i].PhysicalAddressLength == sizeof(AdapterAddress) &&
            !memcmp(Table->Table[i].PhysicalAddress, AdapterAddress, sizeof(AdapterAddress)))
        {
            *Row = Table->Table[i];
            Found = TRUE;
            break;
        }
    }
    FreeMibTable(Table);
    return Found;
}

static
void
ReportInterfaceTable(void)
{
    MIB_IF_TABLE2 *Table;
    ULONG i;

    if (GetIfTable2(&Table) != NO_ERROR)
    {
        Report("NETCXTEST: GetIfTable2 failed");
        return;
    }
    for (i = 0; i < Table->NumEntries; i++)
    {
        const MIB_IF_ROW2 *Entry = &Table->Table[i];

        Report("NETCXTEST: if %lu type %lu oper %d len %lu mac %02x:%02x:%02x:%02x:%02x:%02x '%ls'",
               Entry->InterfaceIndex, Entry->Type, Entry->OperStatus, Entry->PhysicalAddressLength,
               Entry->PhysicalAddress[0], Entry->PhysicalAddress[1], Entry->PhysicalAddress[2],
               Entry->PhysicalAddress[3], Entry->PhysicalAddress[4], Entry->PhysicalAddress[5],
               Entry->Description);
    }
    FreeMibTable(Table);
}

static
void
ReportInterface(const char *When, const MIB_IF_ROW2 *Row)
{
    Report("NETCXTEST: %s: index %lu type %lu oper %d media %d mtu %lu speed %I64u '%ls'",
           When, Row->InterfaceIndex, Row->Type, Row->OperStatus, Row->MediaConnectState,
           Row->Mtu, Row->TransmitLinkSpeed, Row->Description);
    Report("NETCXTEST: %s: in %I64u/%I64u pkts %I64u bytes, out %I64u/%I64u pkts %I64u bytes, in discards %I64u errors %I64u, out errors %I64u",
           When, Row->InUcastPkts, Row->InNUcastPkts, Row->InOctets,
           Row->OutUcastPkts, Row->OutNUcastPkts, Row->OutOctets,
           Row->InDiscards, Row->InErrors, Row->OutErrors);
}

int
wmain(int argc, WCHAR **argv)
{
    WCHAR InfPath[MAX_PATH];
    MIB_UNICASTIPADDRESS_ROW Address;
    MIB_IF_ROW2 Row;
    UCHAR Reply[sizeof(ICMP_ECHO_REPLY) + 64 + 8];
    char Data[32];
    HANDLE Icmp;
    DWORD Status;
    WCHAR *Slash;
    int Replies = 0;
    int i;

    if (argc > 1)
    {
        lstrcpynW(InfPath, argv[1], ARRAYSIZE(InfPath));
    }
    else
    {
        GetModuleFileNameW(NULL, InfPath, ARRAYSIZE(InfPath));
        Slash = wcsrchr(InfPath, L'\\');
        if (Slash)
            Slash[1] = 0;
        lstrcatW(InfPath, L"netcxloop.inf");
    }

    Check(InstallDevice(InfPath), "install");

    for (i = 0; i < 100 && !FindInterface(&Row); i++)
    {
        if (i % 10 == 9)
            Report("NETCXTEST: waiting for the interface, %d ms", (i + 1) * 100);
        Sleep(100);
    }
    Check(i < 100, "interface present");
    if (i == 100)
    {
        ReportInterfaceTable();
        Report("NETCXTEST: DONE %d failures", Failures);
        return 1;
    }

    for (i = 0; i < 50; i++)
    {
        if (GetIfEntry2(&Row) == NO_ERROR && Row.OperStatus == IfOperStatusUp)
            break;
        if (i % 10 == 9)
            Report("NETCXTEST: waiting for oper status up, %d ms", (i + 1) * 100);
        Sleep(100);
    }
    ReportInterface("start", &Row);
    Check(Row.OperStatus == IfOperStatusUp, "oper status up");
    Check(Row.MediaConnectState == MediaConnectStateConnected, "media connected");
    Check(Row.Type == IF_TYPE_ETHERNET_CSMACD, "ethernet type");
    Check(Row.Mtu == 1500, "mtu 1500");
    Check(Row.TransmitLinkSpeed == 1000000000ULL, "link speed 1 Gbps");

    InitializeUnicastIpAddressEntry(&Address);
    Address.InterfaceLuid = Row.InterfaceLuid;
    Address.Address.Ipv4.sin_family = AF_INET;
    Address.Address.Ipv4.sin_addr.s_addr = inet_addr("10.231.0.1");
    Address.OnLinkPrefixLength = 24;
    Status = CreateUnicastIpAddressEntry(&Address);
    Report("NETCXTEST: CreateUnicastIpAddressEntry %lu", Status);
    Check(Status == NO_ERROR, "address created");

    for (i = 0; i < 50; i++)
    {
        if (GetUnicastIpAddressEntry(&Address) == NO_ERROR && Address.DadState == IpDadStatePreferred)
            break;
        if (i % 10 == 9)
            Report("NETCXTEST: waiting for the address, %d ms", (i + 1) * 100);
        Sleep(100);
    }
    Report("NETCXTEST: address DAD state %d", Address.DadState);
    Check(Address.DadState == IpDadStatePreferred, "address preferred");

    Icmp = IcmpCreateFile();
    Check(Icmp != INVALID_HANDLE_VALUE, "icmp handle");
    memset(Data, 'L', sizeof(Data));
    for (i = 0; i < 4 && Icmp != INVALID_HANDLE_VALUE; i++)
    {
        PICMP_ECHO_REPLY Echo = (PICMP_ECHO_REPLY)Reply;

        Status = IcmpSendEcho(Icmp, inet_addr("10.231.0.2"), Data, sizeof(Data), NULL, Reply, sizeof(Reply), 1000);
        if (Status && Echo->Status == IP_SUCCESS && Echo->DataSize == sizeof(Data) &&
            !memcmp(Echo->Data, Data, sizeof(Data)))
            Replies++;
        Report("NETCXTEST: echo %d: count %lu status %lu rtt %lu size %u error %lu",
               i, Status, Status ? Echo->Status : 0, Status ? Echo->RoundTripTime : 0,
               Status ? Echo->DataSize : 0, Status ? 0 : GetLastError());
    }
    if (Icmp != INVALID_HANDLE_VALUE)
        IcmpCloseHandle(Icmp);
    Check(Replies >= 3, "peer answers echo");

    if (GetIfEntry2(&Row) == NO_ERROR)
        ReportInterface("end", &Row);
    Check(Row.InOctets > 0 && Row.OutOctets > 0, "traffic counted");

    Report("NETCXTEST: DONE %d failures", Failures);
    return Failures ? 1 : 0;
}
