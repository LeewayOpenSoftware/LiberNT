/*
 * PROJECT:     LiberNT WiFiCx
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Wi-Fi WDF class extension: client binding, device bring-up and WDI command transport
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif193.001@gmail.com>
 */

#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>
#include <wificx.h>
#include <wificxpoweroffload.h>
#include <wificxpoweroffloadlist.h>
#include <wificxwakesource.h>
#include <wificxwakesourcelist.h>
#include <wdfcx.h>
#include <wdfldr.h>
#include <netadapter_p.h>

extern "C"
NTSTATUS
NTAPI
NdisConvertNdisStatusToNtStatus(
    _In_ NDIS_STATUS Status);

typedef WDFOBJECT (NTAPI *PFN_NETADAPTERGETPARENT)(PNET_DRIVER_GLOBALS, NETADAPTER);
typedef NTSTATUS (NTAPI *PFN_NETADAPTERINITALLOCATECONTEXT)(PNET_DRIVER_GLOBALS, NETADAPTER_INIT *, WDF_OBJECT_ATTRIBUTES *, void **);
typedef void * (NTAPI *PFN_NETADAPTERINITGETTYPEDCONTEXTWORKER)(PNET_DRIVER_GLOBALS, NETADAPTER_INIT *, CONST WDF_OBJECT_CONTEXT_TYPE_INFO *);
typedef NTSTATUS (NTAPI *PFN_NETDRIVEREXTENSIONINITIALIZE)(PNET_DRIVER_GLOBALS, WDFDRIVER, const NET_DRIVER_EXTENSION_CONFIG *);

#define WX_TAG 'xCiW'
#define WX_COMMAND_TIMEOUT_MS 5000
#define WX_TASK_TIMEOUT_MS 30000
#define WX_RESPONSE_BUFFER_SIZE 4096
#define WX_DEFAULT_PORT_ID 0

typedef struct _WX_TASK
{
    LIST_ENTRY Link;
    UINT32 TransactionId;
    UINT16 MessageId;
    NDIS_STATUS Status;
    KEVENT Done;
} WX_TASK;

typedef struct _WX_DEVICE
{
    WDFDEVICE Device;
    WIFI_DEVICE_CONFIG Config;
    WIFI_DEVICE_CAPABILITIES DeviceCapabilities;
    WIFI_STATION_CAPABILITIES StationCapabilities;
    WIFI_WIFIDIRECT_CAPABILITIES WifiDirectCapabilities;
    WIFI_BAND_CAPABILITIES BandCapabilities;
    WIFI_PHY_CAPABILITIES PhyCapabilities;
    NETADAPTER_INIT *CreatingAdapterInit;
    WIFI_ADAPTER_TYPE CreatingAdapterType;
    NETADAPTER StationAdapter;
    volatile LONG NextTransactionId;
    KSPIN_LOCK TaskLock;
    LIST_ENTRY Tasks;
} WX_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WX_DEVICE, WxGetDevice);

typedef struct _WX_ADAPTER
{
    WDFDEVICE Device;
    ULONG PortId;
    WIFI_ADAPTER_TYPE Type;
} WX_ADAPTER;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WX_ADAPTER, WxGetAdapter);

typedef struct _WX_ADAPTER_INIT
{
    WIFI_ADAPTER_TYPE Type;
} WX_ADAPTER_INIT;

WDF_DECLARE_CONTEXT_TYPE(WX_ADAPTER_INIT);

typedef struct _WX_REQUEST
{
    PUCHAR Buffer;
    UINT InputLength;
    UINT OutputLength;
    UINT16 MessageId;
    UINT BytesNeeded;
    NTSTATUS Status;
    UINT BytesWritten;
    KEVENT Done;
} WX_REQUEST;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WX_REQUEST, WxGetRequest);

static EVT_WDFCX_DEVICE_POST_PREPARE_HARDWARE WxEvtPostPrepareHardware;

static
WDFOBJECT
WxNetAdapterGetParent(
    _In_ NETADAPTER Adapter)
{
    return ((PFN_NETADAPTERGETPARENT)NetFunctions[NetAdapterGetParentTableIndex])(NetDriverGlobals, Adapter);
}

static
WX_ADAPTER_INIT *
WxGetAdapterInit(
    _In_ NETADAPTER_INIT *AdapterInit)
{
    return (WX_ADAPTER_INIT *)((PFN_NETADAPTERINITGETTYPEDCONTEXTWORKER)NetFunctions[NetAdapterInitGetTypedContextWorkerTableIndex])(
        NetDriverGlobals, AdapterInit, WDF_GET_CONTEXT_TYPE_INFO(WX_ADAPTER_INIT));
}

static
WX_DEVICE *
WxTryGetDevice(
    _In_ WDFDEVICE Device)
{
    return (WX_DEVICE *)WdfObjectGetTypedContextWorker(Device, WDF_GET_CONTEXT_TYPE_INFO(WX_DEVICE));
}

static
VOID
WxTlvAppend(
    _Inout_updates_bytes_(Capacity) PUCHAR Buffer,
    _In_ ULONG Capacity,
    _Inout_ PULONG Length,
    _In_ UINT16 Type,
    _In_reads_bytes_(ValueLength) const void *Value,
    _In_ UINT16 ValueLength)
{
    if (*Length + sizeof(UINT16) * 2 + ValueLength > Capacity)
        return;

    RtlCopyMemory(Buffer + *Length, &Type, sizeof(Type));
    RtlCopyMemory(Buffer + *Length + sizeof(UINT16), &ValueLength, sizeof(ValueLength));
    RtlCopyMemory(Buffer + *Length + sizeof(UINT16) * 2, Value, ValueLength);
    *Length += sizeof(UINT16) * 2 + ValueLength;
}

static
NTSTATUS
WxSendCommand(
    _In_ WX_DEVICE *Dev,
    _In_ UINT16 MessageId,
    _In_ UINT16 PortId,
    _In_reads_bytes_(TlvLength) const UCHAR *Tlvs,
    _In_ ULONG TlvLength,
    _In_ BOOLEAN Task)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDI_MESSAGE_HEADER *Header;
    LARGE_INTEGER Timeout;
    WX_REQUEST *Request;
    WDFOBJECT Object;
    WX_TASK TaskEntry;
    ULONG BufferSize;
    PUCHAR Buffer;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (Dev->Config.SendCommand == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    BufferSize = sizeof(WDI_MESSAGE_HEADER) + TlvLength;
    if (BufferSize < WX_RESPONSE_BUFFER_SIZE)
        BufferSize = WX_RESPONSE_BUFFER_SIZE;
    Buffer = (PUCHAR)ExAllocatePoolZero(NonPagedPoolNx, BufferSize, WX_TAG);
    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Header = (WDI_MESSAGE_HEADER *)Buffer;
    Header->PortId = PortId;
    Header->TransactionId = (UINT32)InterlockedIncrement(&Dev->NextTransactionId);
    RtlCopyMemory(Buffer + sizeof(WDI_MESSAGE_HEADER), Tlvs, TlvLength);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WX_REQUEST);
    Attributes.ParentObject = Dev->Device;
    Status = WdfObjectCreate(&Attributes, &Object);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Buffer, WX_TAG);
        return Status;
    }

    Request = WxGetRequest(Object);
    Request->Buffer = Buffer;
    Request->InputLength = sizeof(WDI_MESSAGE_HEADER) + TlvLength;
    Request->OutputLength = BufferSize;
    Request->MessageId = MessageId;
    Request->Status = STATUS_PENDING;
    KeInitializeEvent(&Request->Done, NotificationEvent, FALSE);

    if (Task)
    {
        TaskEntry.TransactionId = Header->TransactionId;
        TaskEntry.MessageId = MessageId;
        TaskEntry.Status = NDIS_STATUS_PENDING;
        KeInitializeEvent(&TaskEntry.Done, NotificationEvent, FALSE);
        KeAcquireSpinLock(&Dev->TaskLock, &OldIrql);
        InsertTailList(&Dev->Tasks, &TaskEntry.Link);
        KeReleaseSpinLock(&Dev->TaskLock, OldIrql);
    }

    Dev->Config.SendCommand(Dev->Device, (WIFIREQUEST)Object);

    Timeout.QuadPart = -(LONGLONG)WX_COMMAND_TIMEOUT_MS * 10000;
    if (KeWaitForSingleObject(&Request->Done, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT)
    {
        /* FIXME: an abandoned command keeps its request object until the client completes it */
        DbgPrint("WifiCx: command %u (transaction %u) did not complete\n", MessageId, Header->TransactionId);
        Status = STATUS_IO_TIMEOUT;
    }
    else
    {
        Status = Request->Status;
        if (NT_SUCCESS(Status) && Request->BytesWritten >= sizeof(WDI_MESSAGE_HEADER))
            Status = NdisConvertNdisStatusToNtStatus(((WDI_MESSAGE_HEADER *)Request->Buffer)->Status);
    }

    if (Task && NT_SUCCESS(Status))
    {
        Timeout.QuadPart = -(LONGLONG)WX_TASK_TIMEOUT_MS * 10000;
        if (KeWaitForSingleObject(&TaskEntry.Done, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT)
        {
            DbgPrint("WifiCx: task %u (transaction %u) did not complete\n", MessageId, TaskEntry.TransactionId);
            Status = STATUS_IO_TIMEOUT;
        }
        else
        {
            Status = NdisConvertNdisStatusToNtStatus(TaskEntry.Status);
        }
    }

    if (Task)
    {
        KeAcquireSpinLock(&Dev->TaskLock, &OldIrql);
        RemoveEntryList(&TaskEntry.Link);
        KeReleaseSpinLock(&Dev->TaskLock, OldIrql);
    }

    if (Status != STATUS_IO_TIMEOUT || KeReadStateEvent(&Request->Done))
    {
        ExFreePoolWithTag(Buffer, WX_TAG);
        WdfObjectDelete(Object);
    }

    return Status;
}

static
NTSTATUS
WxInitializeAdapterState(
    _In_ WX_DEVICE *Dev)
{
    static const UCHAR BarMap[][3] =
    {
        { 0, 0, 0 },
        { 1, 24, 1 },
        { 25, 49, 2 },
        { 50, 74, 3 },
        { 75, 100, 4 },
    };
    UCHAR Tlvs[64];
    ULONG Length = 0;
    UCHAR PldrSupport = FALSE;
    UCHAR RadioOn = 1;
    NTSTATUS Status;

    WxTlvAppend(Tlvs, sizeof(Tlvs), &Length, WDI_TLV_LINK_QUALITY_BAR_MAP, BarMap, sizeof(BarMap));
    if (Dev->Config.WdiVersion >= WDI_VERSION_1_0_10)
        WxTlvAppend(Tlvs, sizeof(Tlvs), &Length, WDI_TLV_PLDR_SUPPORT, &PldrSupport, sizeof(PldrSupport));
    Status = WxSendCommand(Dev, WDI_SET_ADAPTER_CONFIGURATION, WDI_PORT_ID_ADAPTER, Tlvs, Length, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    Length = 0;
    WxTlvAppend(Tlvs, sizeof(Tlvs), &Length, WDI_TLV_RADIO_STATE_PARAMETERS, &RadioOn, sizeof(RadioOn));
    return WxSendCommand(Dev, WDI_TASK_SET_RADIO_STATE, WDI_PORT_ID_ADAPTER, Tlvs, Length, TRUE);
}

static
NTSTATUS
WxEvtPostPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    WX_DEVICE *Dev = WxTryGetDevice(Device);
    WDF_OBJECT_ATTRIBUTES Attributes;
    NETADAPTER_INIT *AdapterInit;
    WX_ADAPTER_INIT *InitContext;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (Dev == NULL || Dev->Config.CreateAdapter == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    if (Dev->StationAdapter != NULL)
        return STATUS_SUCCESS;

    AdapterInit = NetAdapterInitAllocate(Device);
    if (AdapterInit == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WX_ADAPTER_INIT);
    Status = ((PFN_NETADAPTERINITALLOCATECONTEXT)NetFunctions[NetAdapterInitAllocateContextTableIndex])(
        NetDriverGlobals, AdapterInit, &Attributes, (void **)&InitContext);
    if (!NT_SUCCESS(Status))
    {
        NetAdapterInitFree(AdapterInit);
        return Status;
    }
    InitContext->Type = WIFI_ADAPTER_EXTENSIBLE_STATION;

    Dev->CreatingAdapterInit = AdapterInit;
    Dev->CreatingAdapterType = InitContext->Type;
    Status = Dev->Config.CreateAdapter(Device, AdapterInit);
    Dev->CreatingAdapterInit = NULL;
    NetAdapterInitFree(AdapterInit);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("WifiCx: EvtWifiDeviceCreateAdapter failed 0x%08lx\n", Status);
        return Status;
    }

    if (Dev->StationAdapter == NULL)
    {
        DbgPrint("WifiCx: the station adapter was not initialized with WifiAdapterInitialize\n");
        return STATUS_INVALID_DEVICE_STATE;
    }

    return WxInitializeAdapterState(Dev);
}

static
NTSTATUS
NTAPI
WxDeviceInitConfig(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDFCX_PNPPOWER_EVENT_CALLBACKS Callbacks;
    PWDFCXDEVICE_INIT CxInit;

    UNREFERENCED_PARAMETER(DriverGlobals);

    CxInit = WdfCxDeviceInitAllocate(DeviceInit);
    if (CxInit == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    WDFCX_PNPPOWER_EVENT_CALLBACKS_INIT(&Callbacks);
    Callbacks.EvtCxDevicePostPrepareHardware = WxEvtPostPrepareHardware;
    WdfCxDeviceInitSetPnpPowerEventCallbacks(CxInit, &Callbacks);
    return STATUS_SUCCESS;
}

static
ULONG
NTAPI
WxDeviceGetOsWdiVersion(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(Device);
    return WDI_VERSION_LATEST;
}

#define WX_STORE_CAPABILITIES(_Field, _Capabilities)                                      \
    do {                                                                                  \
        WX_DEVICE *Dev_ = WxTryGetDevice(Device);                                         \
        if (Dev_ == NULL || (_Capabilities) == NULL)                                      \
            return STATUS_INVALID_PARAMETER;                                              \
        RtlZeroMemory(&Dev_->_Field, sizeof(Dev_->_Field));                               \
        RtlCopyMemory(&Dev_->_Field, (_Capabilities),                                     \
                      min((_Capabilities)->Size, (ULONG)sizeof(Dev_->_Field)));           \
        return STATUS_SUCCESS;                                                            \
    } while (0)

/* TODO: deep-copy the arrays the capability structures point to */

static
NTSTATUS
NTAPI
WxDeviceSetDeviceCapabilities(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ CONST WIFI_DEVICE_CAPABILITIES *Capabilities)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    WX_STORE_CAPABILITIES(DeviceCapabilities, Capabilities);
}

static
NTSTATUS
NTAPI
WxDeviceSetStationCapabilities(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ CONST WIFI_STATION_CAPABILITIES *Capabilities)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    WX_STORE_CAPABILITIES(StationCapabilities, Capabilities);
}

static
NTSTATUS
NTAPI
WxDeviceSetWiFiDirectCapabilities(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ CONST WIFI_WIFIDIRECT_CAPABILITIES *Capabilities)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    WX_STORE_CAPABILITIES(WifiDirectCapabilities, Capabilities);
}

static
NTSTATUS
NTAPI
WxDeviceSetBandCapabilities(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ CONST WIFI_BAND_CAPABILITIES *Capabilities)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    WX_STORE_CAPABILITIES(BandCapabilities, Capabilities);
}

static
NTSTATUS
NTAPI
WxDeviceSetPhyCapabilities(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ CONST WIFI_PHY_CAPABILITIES *Capabilities)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    WX_STORE_CAPABILITIES(PhyCapabilities, Capabilities);
}

static
NTSTATUS
NTAPI
WxDeviceInitialize(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ WIFI_DEVICE_CONFIG *Config)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WX_DEVICE *Dev;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DriverGlobals);

    if (Config == NULL || Config->Size < sizeof(WIFI_DEVICE_CONFIG) ||
        Config->SendCommand == NULL || Config->CreateAdapter == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WX_DEVICE);
    Status = WdfObjectAllocateContext(Device, &Attributes, (PVOID *)&Dev);
    if (!NT_SUCCESS(Status))
        return Status;

    Dev->Device = Device;
    RtlCopyMemory(&Dev->Config, Config, sizeof(WIFI_DEVICE_CONFIG));
    KeInitializeSpinLock(&Dev->TaskLock);
    InitializeListHead(&Dev->Tasks);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
WxDeviceReceiveIndication(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ UINT16 MessageId,
    _In_ WDFMEMORY Data)
{
    WX_DEVICE *Dev = WxTryGetDevice(Device);
    WDI_MESSAGE_HEADER *Header;
    PLIST_ENTRY Entry;
    size_t Length;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DriverGlobals);

    if (Dev == NULL || Data == NULL)
        return;

    Header = (WDI_MESSAGE_HEADER *)WdfMemoryGetBuffer(Data, &Length);
    if (Header == NULL || Length < sizeof(WDI_MESSAGE_HEADER))
        return;

    if (Header->TransactionId != 0)
    {
        KeAcquireSpinLock(&Dev->TaskLock, &OldIrql);
        for (Entry = Dev->Tasks.Flink; Entry != &Dev->Tasks; Entry = Entry->Flink)
        {
            WX_TASK *Task = CONTAINING_RECORD(Entry, WX_TASK, Link);

            if (Task->TransactionId == Header->TransactionId && Task->MessageId == MessageId)
            {
                Task->Status = Header->Status;
                KeSetEvent(&Task->Done, IO_NO_INCREMENT, FALSE);
                break;
            }
        }
        KeReleaseSpinLock(&Dev->TaskLock, OldIrql);
        return;
    }

    /* TODO: route unsolicited indications to the WDI engine */
}

static
NTSTATUS
NTAPI
WxAdapterInitialize(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WX_ADAPTER *Context;
    WDFOBJECT Parent;
    WX_DEVICE *Dev;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DriverGlobals);

    Parent = WxNetAdapterGetParent(Adapter);
    Dev = Parent ? WxTryGetDevice((WDFDEVICE)Parent) : NULL;
    if (Dev == NULL || Dev->CreatingAdapterInit == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WX_ADAPTER);
    Status = WdfObjectAllocateContext(Adapter, &Attributes, (PVOID *)&Context);
    if (!NT_SUCCESS(Status))
        return Status;

    Context->Device = Dev->Device;
    Context->Type = Dev->CreatingAdapterType;
    /* FIXME: the default station port number is assumed to be 0 */
    Context->PortId = WX_DEFAULT_PORT_ID;
    if (Context->Type == WIFI_ADAPTER_EXTENSIBLE_STATION)
        Dev->StationAdapter = Adapter;
    return STATUS_SUCCESS;
}

static
PVOID
NTAPI
WxRequestGetInOutBuffer(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIREQUEST Request,
    _Out_opt_ UINT *InputBufferLength,
    _Out_opt_ UINT *OutBufferLength)
{
    WX_REQUEST *Context = WxGetRequest((WDFOBJECT)Request);

    UNREFERENCED_PARAMETER(DriverGlobals);

    if (InputBufferLength)
        *InputBufferLength = Context->InputLength;
    if (OutBufferLength)
        *OutBufferLength = Context->OutputLength;
    return Context->Buffer;
}

static
UINT16
NTAPI
WxRequestGetMessageId(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIREQUEST Request)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    return WxGetRequest((WDFOBJECT)Request)->MessageId;
}

static
VOID
NTAPI
WxRequestSetBytesNeeded(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIREQUEST Request,
    _In_ UINT BytesNeeded)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    WxGetRequest((WDFOBJECT)Request)->BytesNeeded = BytesNeeded;
}

static
VOID
NTAPI
WxRequestComplete(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIREQUEST Request,
    _In_ NTSTATUS NtStatus,
    _In_ UINT BytesWritten)
{
    WX_REQUEST *Context = WxGetRequest((WDFOBJECT)Request);

    UNREFERENCED_PARAMETER(DriverGlobals);

    Context->Status = NtStatus;
    Context->BytesWritten = BytesWritten;
    KeSetEvent(&Context->Done, IO_NO_INCREMENT, FALSE);
}

static
VOID
NTAPI
WxAdapterPowerOffloadSetRsnRekeyCapabilities(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST WIFI_ADAPTER_POWER_OFFLOAD_RSN_REKEY_CAPABILITIES *Capabilities)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Capabilities);
    /* TODO: RSN rekey power offload capabilities */
}

static
VOID
NTAPI
WxAdapterSetWakeCapabilities(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST WIFI_ADAPTER_WAKE_CAPABILITIES *Capabilities)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Capabilities);
    /* TODO: Wi-Fi wake capabilities */
}

static
VOID
NTAPI
WxAdapterReportWakeReason(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ WIFI_WAKE_REASON_TYPE Reason)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Reason);
    /* TODO: Wi-Fi wake reasons */
}

static
NTSTATUS
NTAPI
WxDirectDeviceCreate(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIDIRECT_DEVICE_INIT *WifiDirectDeviceInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *WifiDirectDeviceAttributes,
    _Out_ WIFIDIRECTDEVICE *WifiDirectDevice)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(WifiDirectDeviceInit);
    UNREFERENCED_PARAMETER(WifiDirectDeviceAttributes);
    *WifiDirectDevice = NULL;
    /* TODO: Wi-Fi Direct devices */
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
NTAPI
WxDirectDeviceInitialize(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIDIRECTDEVICE WifiDirectDevice)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(WifiDirectDevice);
    /* TODO: Wi-Fi Direct devices */
    return STATUS_NOT_SUPPORTED;
}

static
ULONG
NTAPI
WxAdapterGetPortId(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    return WxGetAdapter(Adapter)->PortId;
}

static
WIFI_ADAPTER_TYPE
NTAPI
WxAdapterInitGetType(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit)
{
    WX_ADAPTER_INIT *Context;

    UNREFERENCED_PARAMETER(DriverGlobals);

    Context = WxGetAdapterInit(AdapterInit);
    return Context ? Context->Type : WIFI_ADAPTER_EXTENSIBLE_STATION;
}

static
WIFI_ADAPTER_TYPE
NTAPI
WxAdapterGetType(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    return WxGetAdapter(Adapter)->Type;
}

static
ULONG
NTAPI
WxDirectDeviceGetPortId(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIDIRECTDEVICE WifiDirectDevice)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(WifiDirectDevice);
    /* TODO: Wi-Fi Direct devices */
    return 0;
}

static
void
NTAPI
WxAdapterInitAddTxDemux(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER_INIT *Adapter,
    _In_ CONST WIFI_ADAPTER_TX_DEMUX *Demux)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Demux);
    /* TODO: peer and WMM transmit demultiplexing */
}

static
NET_EUI48_ADDRESS
NTAPI
WxTxQueueGetDemuxPeerAddress(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE Queue)
{
    NET_EUI48_ADDRESS Address;

    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(Queue);
    RtlZeroMemory(&Address, sizeof(Address));
    /* TODO: peer and WMM transmit demultiplexing */
    return Address;
}

static
UINT8
NTAPI
WxTxQueueGetDemuxWmmInfo(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPACKETQUEUE Queue)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(Queue);
    /* TODO: peer and WMM transmit demultiplexing */
    return 0;
}

static
void
NTAPI
WxAdapterAddPeer(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_EUI48_ADDRESS *Address)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Address);
    /* TODO: peer and WMM transmit demultiplexing */
}

static
void
NTAPI
WxAdapterRemovePeer(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_EUI48_ADDRESS *Address)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Address);
    /* TODO: peer and WMM transmit demultiplexing */
}

static
WIFI_POWER_OFFLOAD_TYPE
NTAPI
WxPowerOffloadGetType(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIPOWEROFFLOAD WifiPowerOffload)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(WifiPowerOffload);
    /* TODO: Wi-Fi power offloads */
    return (WIFI_POWER_OFFLOAD_TYPE)0;
}

static
NETADAPTER
NTAPI
WxPowerOffloadGetAdapter(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIPOWEROFFLOAD WifiPowerOffload)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(WifiPowerOffload);
    /* TODO: Wi-Fi power offloads */
    return NULL;
}

static
void
NTAPI
WxPowerOffloadGet80211RSNRekeyParameters(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIPOWEROFFLOAD WifiPowerOffload,
    _Inout_ WIFI_POWER_OFFLOAD_80211RSNREKEY_PARAMETERS *Parameters)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(WifiPowerOffload);
    UNREFERENCED_PARAMETER(Parameters);
    /* TODO: Wi-Fi power offloads */
}

static
void
NTAPI
WxDeviceGetPowerOffloadList(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _Inout_ WIFI_POWER_OFFLOAD_LIST *List)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(List);
    /* TODO: Wi-Fi power offloads */
}

static
SIZE_T
NTAPI
WxPowerOffloadListGetCount(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ CONST WIFI_POWER_OFFLOAD_LIST *List)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(List);
    /* TODO: Wi-Fi power offloads */
    return 0;
}

static
WIFIPOWEROFFLOAD
NTAPI
WxPowerOffloadListGetElement(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ CONST WIFI_POWER_OFFLOAD_LIST *List,
    _In_ SIZE_T Index)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(List);
    UNREFERENCED_PARAMETER(Index);
    /* TODO: Wi-Fi power offloads */
    return NULL;
}

static
WIFI_WAKE_SOURCE_TYPE
NTAPI
WxWakeSourceGetType(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIWAKESOURCE WakeSource)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(WakeSource);
    /* TODO: Wi-Fi wake sources */
    return (WIFI_WAKE_SOURCE_TYPE)0;
}

static
NETADAPTER
NTAPI
WxWakeSourceGetAdapter(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIWAKESOURCE WakeSource)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(WakeSource);
    /* TODO: Wi-Fi wake sources */
    return NULL;
}

static
void
NTAPI
WxDeviceGetWakeSourceList(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _Inout_ WIFI_WAKE_SOURCE_LIST *List)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(List);
    /* TODO: Wi-Fi wake sources */
}

static
SIZE_T
NTAPI
WxWakeSourceListGetCount(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ CONST WIFI_WAKE_SOURCE_LIST *List)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(List);
    /* TODO: Wi-Fi wake sources */
    return 0;
}

static
WIFIWAKESOURCE
NTAPI
WxWakeSourceListGetElement(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ CONST WIFI_WAKE_SOURCE_LIST *List,
    _In_ SIZE_T Index)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(List);
    UNREFERENCED_PARAMETER(Index);
    /* TODO: Wi-Fi wake sources */
    return NULL;
}

static
void
NTAPI
WxPowerOffloadGetActionFrameWakePatternParameters(
    _In_ PWIFI_DRIVER_GLOBALS DriverGlobals,
    _In_ WIFIPOWEROFFLOAD WifiPowerOffload,
    _Inout_ WIFI_POWER_OFFLOAD_ACTION_FRAME_WAKE_PATTERN_PARAMETERS *Parameters)
{
    UNREFERENCED_PARAMETER(DriverGlobals);
    UNREFERENCED_PARAMETER(WifiPowerOffload);
    UNREFERENCED_PARAMETER(Parameters);
    /* TODO: Wi-Fi power offloads */
}

static PVOID const WxFunctions[] =
{
    (PVOID)WxDeviceInitConfig,
    (PVOID)WxDeviceGetOsWdiVersion,
    (PVOID)WxDeviceSetDeviceCapabilities,
    (PVOID)WxDeviceSetStationCapabilities,
    (PVOID)WxDeviceSetWiFiDirectCapabilities,
    (PVOID)WxDeviceSetBandCapabilities,
    (PVOID)WxDeviceSetPhyCapabilities,
    (PVOID)WxDeviceInitialize,
    (PVOID)WxDeviceReceiveIndication,
    (PVOID)WxAdapterInitialize,
    (PVOID)WxRequestGetInOutBuffer,
    (PVOID)WxRequestGetMessageId,
    (PVOID)WxRequestSetBytesNeeded,
    (PVOID)WxRequestComplete,
    (PVOID)WxAdapterPowerOffloadSetRsnRekeyCapabilities,
    (PVOID)WxAdapterSetWakeCapabilities,
    (PVOID)WxAdapterReportWakeReason,
    (PVOID)WxDirectDeviceCreate,
    (PVOID)WxDirectDeviceInitialize,
    (PVOID)WxAdapterGetPortId,
    (PVOID)WxAdapterInitGetType,
    (PVOID)WxAdapterGetType,
    (PVOID)WxDirectDeviceGetPortId,
    (PVOID)WxAdapterInitAddTxDemux,
    (PVOID)WxTxQueueGetDemuxPeerAddress,
    (PVOID)WxTxQueueGetDemuxWmmInfo,
    (PVOID)WxAdapterAddPeer,
    (PVOID)WxAdapterRemovePeer,
    (PVOID)WxPowerOffloadGetType,
    (PVOID)WxPowerOffloadGetAdapter,
    (PVOID)WxPowerOffloadGet80211RSNRekeyParameters,
    (PVOID)WxDeviceGetPowerOffloadList,
    (PVOID)WxPowerOffloadListGetCount,
    (PVOID)WxPowerOffloadListGetElement,
    (PVOID)WxWakeSourceGetType,
    (PVOID)WxWakeSourceGetAdapter,
    (PVOID)WxDeviceGetWakeSourceList,
    (PVOID)WxWakeSourceListGetCount,
    (PVOID)WxWakeSourceListGetElement,
    (PVOID)WxPowerOffloadGetActionFrameWakePatternParameters,
};

C_ASSERT(RTL_NUMBER_OF(WxFunctions) == WifiFunctionTableNumEntries);

static
NTSTATUS
NTAPI
WxBindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ClientGlobals)
{
    if (ClassBindInfo == NULL || ClassBindInfo->Size < sizeof(WDF_CLASS_BIND_INFO))
        return STATUS_INVALID_PARAMETER;

    if (ClassBindInfo->Version.Major != WIFI_VERSION_MAJOR ||
        ClassBindInfo->Version.Minor > WIFI_VERSION_MINOR ||
        ClassBindInfo->FunctionTable == NULL ||
        ClassBindInfo->FunctionTableCount > RTL_NUMBER_OF(WxFunctions))
    {
        return STATUS_NOT_SUPPORTED;
    }

    RtlCopyMemory(ClassBindInfo->FunctionTable, WxFunctions,
                  ClassBindInfo->FunctionTableCount * sizeof(WxFunctions[0]));
    if (ClassBindInfo->ClassBindInfo != NULL)
        *(PVOID *)ClassBindInfo->ClassBindInfo = ClientGlobals;
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
WxUnbindClient(
    _In_ PWDF_CLASS_BIND_INFO ClassBindInfo,
    _In_ PWDF_COMPONENT_GLOBALS ClientGlobals)
{
    UNREFERENCED_PARAMETER(ClientGlobals);

    if (ClassBindInfo != NULL && ClassBindInfo->ClassBindInfo != NULL)
        *(PVOID *)ClassBindInfo->ClassBindInfo = NULL;
}

static WDF_CLASS_LIBRARY_INFO WxLibraryInfo =
{
    sizeof(WDF_CLASS_LIBRARY_INFO),
    { WIFI_VERSION_MAJOR, WIFI_VERSION_MINOR, 0 },
    NULL,
    NULL,
    WxBindClient,
    WxUnbindClient,
};

extern "C"
NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    DECLARE_CONST_UNICODE_STRING(Sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\WiFiCx");
    NET_DRIVER_EXTENSION_CONFIG ExtensionConfig;
    WDF_DRIVER_CONFIG Config;
    PWDFDEVICE_INIT DeviceInit;
    WDFDRIVER Driver;
    WDFDEVICE Control;
    NTSTATUS Status;

    WDF_DRIVER_CONFIG_INIT(&Config, NULL);
    Config.DriverInitFlags = WdfDriverInitNonPnpDriver;
    Config.DriverPoolTag = WX_TAG;
    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&ExtensionConfig, sizeof(ExtensionConfig));
    ExtensionConfig.Size = sizeof(ExtensionConfig);
    ExtensionConfig.AllowNetAdapterCreation = FALSE;
    Status = ((PFN_NETDRIVEREXTENSIONINITIALIZE)NetFunctions[NetDriverExtensionInitializeTableIndex])(
        NetDriverGlobals, Driver, &ExtensionConfig);
    if (!NT_SUCCESS(Status))
        return Status;

    DeviceInit = WdfControlDeviceInitAllocate(Driver, &Sddl);
    if (DeviceInit == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = WdfDeviceInitAssignName(DeviceInit, &Name);
    if (NT_SUCCESS(Status))
        Status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &Control);
    if (!NT_SUCCESS(Status))
    {
        if (DeviceInit != NULL)
            WdfDeviceInitFree(DeviceInit);
        return Status;
    }
    WdfControlFinishInitializing(Control);

    return WdfRegisterClassLibrary(&WxLibraryInfo, RegistryPath, &Name);
}
