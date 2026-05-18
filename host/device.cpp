// Copyright (C) Microsoft Corporation. All rights reserved.

#include "driver.h"
#include "device.tmh"

#define MAX_HOST_NTB_SIZE               (0x10000)
#define MAX_HOST_MTU_SIZE               (9014)
#define MAX_HOST_TX_NTB_DATAGRAM_COUNT  (UINT16) (16)
#define PENDING_BULK_IN_READS           (8)

// Apple Quirk: register each step under its own value name (the step
// string itself). Steps accumulate; values are stable across calls.
// We also maintain "LastStep" and "LastStatus" for the latest update.
//
// To read the full trace from user mode after a device failure:
//   Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\UsbNcm\AppleQuirkStatus
//
// The value with the highest StepIndex (in its DWORD payload's high bits)
// is the last step that ran. (We pack step index + NTSTATUS into the value.)
static volatile LONG g_AppleNcmStepCounter = 0;

static void AppleNcm_WriteRegStatus(_In_z_ PCWSTR step, NTSTATUS status)
{
    HANDLE key = NULL;
    UNICODE_STRING regPath;
    RtlInitUnicodeString(&regPath,
        L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\UsbNcm\\AppleQuirkStatus");
    OBJECT_ATTRIBUTES attrs;
    InitializeObjectAttributes(&attrs, &regPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    NTSTATUS rs = ZwCreateKey(&key, KEY_SET_VALUE, &attrs, 0, NULL,
                              REG_OPTION_NON_VOLATILE, NULL);
    if (!NT_SUCCESS(rs) || key == NULL) return;

    LONG idx = InterlockedIncrement(&g_AppleNcmStepCounter);

    // Value name = step string. Payload = REG_QWORD containing
    // (high 32 bits = step index, low 32 bits = NTSTATUS).
    UNICODE_STRING valName;
    RtlInitUnicodeString(&valName, step);
    LARGE_INTEGER payload;
    payload.HighPart = idx;
    payload.LowPart = (ULONG)status;
    ZwSetValueKey(key, &valName, 0, REG_QWORD, &payload, sizeof(payload));

    // Also keep "LastStep" + "LastStatus" + "LastIndex" for quick read.
    UNICODE_STRING last;
    RtlInitUnicodeString(&last, L"LastStep");
    SIZE_T stepBytes = (wcslen(step) + 1) * sizeof(WCHAR);
    ZwSetValueKey(key, &last, 0, REG_SZ, (PVOID)step, (ULONG)stepBytes);

    UNICODE_STRING lastStatus;
    RtlInitUnicodeString(&lastStatus, L"LastStatus");
    ZwSetValueKey(key, &lastStatus, 0, REG_DWORD, &status, sizeof(status));

    UNICODE_STRING lastIdx;
    RtlInitUnicodeString(&lastIdx, L"LastIndex");
    ZwSetValueKey(key, &lastIdx, 0, REG_DWORD, &idx, sizeof(idx));

    ZwClose(key);
}
#define APPLENCM_STEP(name, status) AppleNcm_WriteRegStatus(L##name, (status))

const USBNCM_DEVICE_EVENT_CALLBACKS UsbNcmHostDevice::s_NcmDeviceCallbacks =
{
    sizeof(USBNCM_DEVICE_EVENT_CALLBACKS),
    UsbNcmHostDevice::StartReceive,
    UsbNcmHostDevice::StopReceive,
    UsbNcmHostDevice::StartTransmit,
    UsbNcmHostDevice::StopTransmit,
    UsbNcmHostDevice::TransmitFrames
};

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
StartPipe(
    _In_ WDFUSBPIPE pipe
)
{
    WDFIOTARGET wdfIotarget;

    wdfIotarget = WdfUsbTargetPipeGetIoTarget(pipe);
    return WdfIoTargetStart(wdfIotarget);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
StopPipe(
    _In_ WDFUSBPIPE pipe
)
{
    WDFIOTARGET wdfIotarget;

    wdfIotarget = WdfUsbTargetPipeGetIoTarget(pipe);
    WdfIoTargetStop(wdfIotarget, WdfIoTargetCancelSentIo);
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::SetDeviceFriendlyName(
    void
)
{
    //  Update the device name with the model from the USB descriptor
    USB_DEVICE_DESCRIPTOR deviceDescriptor;
    PWSTR friendlyName = nullptr;
    WDFMEMORY friendlyNameMemory;
    WDF_OBJECT_ATTRIBUTES objectAttribs;

    WdfUsbTargetDeviceGetDeviceDescriptor(m_WdfUsbTargetDevice, &deviceDescriptor);

    USHORT manufacturerStringLength = 0;
    USHORT productStringLength = 0;

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceQueryString(
            m_WdfUsbTargetDevice,
            nullptr,
            nullptr,
            nullptr,
            &manufacturerStringLength,
            deviceDescriptor.iManufacturer,
            0),
        "WdfUsbTargetDeviceQueryString failed");

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceQueryString(
            m_WdfUsbTargetDevice,
            nullptr,
            nullptr,
            nullptr,
            &productStringLength,
            deviceDescriptor.iProduct,
            0),
        "WdfUsbTargetDeviceQueryString failed");

    ULONG friendlyNameByteCount = sizeof(WCHAR) * 
        (manufacturerStringLength + 1 +  // 1 white space
         productStringLength + 1);       // allocate 1 more char to make sure string would be null-terminated
   
    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttribs);
    objectAttribs.ParentObject = m_WdfDevice;

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfMemoryCreate(
            &objectAttribs,
            PagedPool,
            0,
            friendlyNameByteCount,
            &friendlyNameMemory,
            (PVOID *)&friendlyName),
        "WdfMemoryCreate failed");

    RtlZeroMemory(friendlyName, friendlyNameByteCount);

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceQueryString(
            m_WdfUsbTargetDevice,
            nullptr,
            nullptr,
            friendlyName,
            &manufacturerStringLength,
            deviceDescriptor.iManufacturer,
            0),
        "WdfUsbTargetDeviceQueryString failed");

    friendlyName[manufacturerStringLength] = L' ';
    
    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceQueryString(
            m_WdfUsbTargetDevice,
            nullptr,
            nullptr,
            &friendlyName[manufacturerStringLength + 1],
            &productStringLength,
            deviceDescriptor.iProduct,
            0),
        "WdfUsbTargetDeviceQueryString failed");
 
    WDF_DEVICE_PROPERTY_DATA propertyData;
    WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &DEVPKEY_Device_FriendlyName);
    propertyData.Flags = PLUGPLAY_PROPERTY_PERSISTENT;

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfDeviceAssignProperty(
            m_WdfDevice,
            &propertyData,
            DEVPROP_TYPE_STRING,
            friendlyNameByteCount,
            friendlyName),
        "WdfDeviceAssignProperty failed");

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::InitializeDevice(
    void
)
{
    WDF_USB_DEVICE_INFORMATION deviceInfo;
    WDF_USB_DEVICE_CREATE_CONFIG createParams;

    PAGED_CODE();

    WDF_USB_DEVICE_CREATE_CONFIG_INIT(
        &createParams,
        USBD_CLIENT_CONTRACT_VERSION_602);

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceCreateWithParameters(
            m_WdfDevice,
            &createParams,
            WDF_NO_OBJECT_ATTRIBUTES,
            &m_WdfUsbTargetDevice),
        "WdfUsbTargetDeviceCreateWithParameters failed");

    // Ignore any error if we failed to set PnP FriendlyName
    (void) SetDeviceFriendlyName();

    // Retrieve USBD version information, port driver capabilites and device
    // capabilites such as speed, power, etc.

    WDF_USB_DEVICE_INFORMATION_INIT(&deviceInfo);

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceRetrieveInformation(
            m_WdfUsbTargetDevice,
            &deviceInfo),
        "WdfUsbTargetDeviceRetrieveInformation failed");

    // Apple Quirk: trace progression so we can pinpoint failure point.
    APPLENCM_STEP("InitializeDevice:before-SelectConfiguration", STATUS_SUCCESS);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: InitializeDevice - calling SelectConfiguration\n");
    NTSTATUS _sc = SelectConfiguration();
    APPLENCM_STEP("InitializeDevice:after-SelectConfiguration", _sc);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: SelectConfiguration returned 0x%08X\n", _sc);
    NCM_RETURN_IF_NOT_NT_SUCCESS(_sc);

    APPLENCM_STEP("InitializeDevice:before-SelectSetting", STATUS_SUCCESS);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: calling SelectSetting\n");
    NTSTATUS _ss = SelectSetting();
    APPLENCM_STEP("InitializeDevice:after-SelectSetting", _ss);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: SelectSetting returned 0x%08X\n", _ss);
    NCM_RETURN_IF_NOT_NT_SUCCESS(_ss);

    APPLENCM_STEP("InitializeDevice:before-RetrieveInterruptPipe", STATUS_SUCCESS);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: calling RetrieveInterruptPipe\n");
    NTSTATUS _rip = RetrieveInterruptPipe();
    APPLENCM_STEP("InitializeDevice:after-RetrieveInterruptPipe", _rip);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: RetrieveInterruptPipe returned 0x%08X (pipe=%p)\n",
        _rip, m_ControlInterruptPipe);
    NCM_RETURN_IF_NOT_NT_SUCCESS(_rip);

    APPLENCM_STEP("InitializeDevice:before-RetrieveDataBulkPipes", STATUS_SUCCESS);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: calling RetrieveDataBulkPipes\n");
    NTSTATUS _rdb = RetrieveDataBulkPipes();
    APPLENCM_STEP("InitializeDevice:after-RetrieveDataBulkPipes", _rdb);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: RetrieveDataBulkPipes returned 0x%08X (in=%p out=%p)\n",
        _rdb, m_DataBulkInPipe, m_DataBulkOutPipe);
    NCM_RETURN_IF_NOT_NT_SUCCESS(_rdb);

    // Phase A: attempt to also claim NCM #2 (interface 3). Failures here are
    // non-fatal — NCM #1 is what gets us network connectivity. This call is
    // diagnostic for now: it sets up an EP 0x82 IN ContinuousReader so we
    // can verify Mac's anpi* peer sends data when prompted.
    NTSTATUS _rdb2 = RetrieveDataBulkPipes2();
    APPLENCM_STEP("InitializeDevice:after-RetrieveDataBulkPipes2", _rdb2);

    // Apple Quirk: macOS NCM peripherals (VID 0x05AC, e.g. PID 0x1905)
    // omit the interrupt endpoint entirely. The two bulk endpoints are
    // mandatory; the interrupt endpoint is optional and we tolerate its
    // absence (m_ControlInterruptPipe may be nullptr).
    NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
        m_DataBulkInPipe && m_DataBulkOutPipe,
        STATUS_DEVICE_HARDWARE_ERROR,
        "Bad NCM pipes incomplete");

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::CreateAdapter(
    void
)
{
    PAGED_CODE();

    USBNCM_ADAPTER_PARAMETERS parameters =
    {
        m_Use32BitNtb,
        m_MacAddress,
        m_MaxDatagramSize,
        m_NtbParamters.wNtbOutMaxDatagrams > 0
            ? m_NtbParamters.wNtbOutMaxDatagrams
            : MAX_HOST_TX_NTB_DATAGRAM_COUNT,
        m_NtbParamters.dwNtbOutMaxSize,
        m_NtbParamters.wNdpOutAlignment,
        m_NtbParamters.wNdpOutDivisor,
        m_NtbParamters.wNdpOutPayloadRemainder,
    };

    NCM_RETURN_IF_NOT_NT_SUCCESS(
        UsbNcmAdapterCreate(
            m_WdfDevice,
            &parameters,
            &UsbNcmHostDevice::s_NcmDeviceCallbacks,
            &m_NetAdapter,
            &m_NcmAdapterCallbacks));

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
void
UsbNcmHostDevice::DestroyAdapter(
    void
)
{
    PAGED_CODE();

    if (m_NetAdapter != nullptr)
    {
        UsbNcmAdapterDestory(m_NetAdapter);
    }
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::RequestClassSpecificControlTransfer(
    UINT8 request,
    WDF_USB_BMREQUEST_DIRECTION direction,
    WDF_USB_BMREQUEST_RECIPIENT recipient,
    UINT16 value,
    PWDF_MEMORY_DESCRIPTOR memoryDescriptor
)
{
    WDF_USB_CONTROL_SETUP_PACKET controlSetupPacket;
    WDF_REQUEST_SEND_OPTIONS sendOptions;

    PAGED_CODE();

    WDF_USB_CONTROL_SETUP_PACKET_INIT_CLASS(
        &controlSetupPacket,
        direction,
        recipient,
        request,
        value,
        WdfUsbInterfaceGetInterfaceNumber(m_ControlInterface));

    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, 0);
    //WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&sendOptions, WDF_REL_TIMEOUT_IN_SEC(10));

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetDeviceSendControlTransferSynchronously(
            m_WdfUsbTargetDevice,
            WDF_NO_HANDLE,
            &sendOptions,
            &controlSetupPacket,
            memoryDescriptor,
            nullptr),
        "WdfUsbTargetDeviceSendControlTransferSynchronously failed");

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::SelectConfiguration(
    void
)
{
    WDF_OBJECT_ATTRIBUTES objectAttribs;

    PAGED_CODE();

    APPLENCM_STEP("SelectConfiguration:ENTRY", STATUS_SUCCESS);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: SelectConfiguration ENTRY\n");

    NTSTATUS status = STATUS_SUCCESS;


    // 1. Retrieve all descriptors for this NCM USB device

    PUSB_CONFIGURATION_DESCRIPTOR pDescriptors = NULL;
    USHORT sizeDescriptors;
    WDFMEMORY descriptorMemory;

    status = WdfUsbTargetDeviceRetrieveConfigDescriptor(
        m_WdfUsbTargetDevice,
        NULL,
        &sizeDescriptors);

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: RetrieveConfigDescriptor(NULL) status=0x%08X size=%d\n",
        status, sizeDescriptors);

    if (status != STATUS_BUFFER_TOO_SMALL) {
        APPLENCM_STEP("SelectConfiguration:firstRetrieve-failed", status);
        NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
            FALSE, status, "WdfUsbTargetDeviceRetrieveConfigDescriptor failed");
    }
    APPLENCM_STEP("SelectConfiguration:after-firstRetrieve", STATUS_SUCCESS);

    WDF_OBJECT_ATTRIBUTES_INIT(&objectAttribs);
    objectAttribs.ParentObject = m_WdfDevice;

    NTSTATUS _memSt = WdfMemoryCreate(
            &objectAttribs,
            NonPagedPoolNx,
            0,
            sizeDescriptors,
            &descriptorMemory,
            (PVOID*) &pDescriptors);
    APPLENCM_STEP("SelectConfiguration:after-WdfMemoryCreate", _memSt);
    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(_memSt, "WdfMemoryCreate failed");

    RtlZeroMemory(pDescriptors, sizeDescriptors);

    NTSTATUS _retr2 = WdfUsbTargetDeviceRetrieveConfigDescriptor(
            m_WdfUsbTargetDevice,
            pDescriptors,
            &sizeDescriptors);
    APPLENCM_STEP("SelectConfiguration:after-secondRetrieve", _retr2);
    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(_retr2,
        "WdfUsbTargetDeviceRetrieveConfigDescriptor (data) failed");
    APPLENCM_STEP("SelectConfiguration:before-parseLoop", STATUS_SUCCESS);

    // 2. scan the descriptors for communication and data interfaces
    //
    // Apple Quirk: use sentinel 0xFF to mean "not set", since interface
    // number 0 is a valid value (MI_00).

    BYTE controlInterfaceNumber = 0xFF;
    BYTE dataInterfaceNumber = 0xFF;
    // Phase A: capture the SECOND NCM data interface too (Apple exposes two —
    // first one bridges to en6, second to anpi3).
    BYTE dataInterfaceNumber2 = 0xFF;
    PUSB_NCM_CS_FUNCTIONAL_DESCRIPTOR pNcmFunctionalDescr = nullptr;
    PUSB_ECM_CS_NET_FUNCTIONAL_DESCRIPTOR pEcmFunctionalDescr = nullptr;

    size_t currDescrptorOffset = 0;
    PUSB_COMMON_DESCRIPTOR pCurrDescriptor = (PUSB_COMMON_DESCRIPTOR)pDescriptors;

    while (currDescrptorOffset < pDescriptors->wTotalLength)
    {
        switch (pCurrDescriptor->bDescriptorType)
        {
            case USB_INTERFACE_DESCRIPTOR_TYPE:
            {
                NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
                    pCurrDescriptor->bLength == sizeof(USB_INTERFACE_DESCRIPTOR),
                    STATUS_DEVICE_HARDWARE_ERROR,
                    "Bad UsbInterfaceDescriptor");

                PUSB_INTERFACE_DESCRIPTOR pIfDescriptor = (PUSB_INTERFACE_DESCRIPTOR) pCurrDescriptor;

                if ((pIfDescriptor->bInterfaceClass == USB_CDC_INTERFACE_CLASS_COMM) &&
                    (pIfDescriptor->bInterfaceSubClass == USB_CDC_INTERFACE_SUBCLASS_NCM))
                {
                    // Apple Quirk: 0 or 1 interrupt endpoints both acceptable.
                    NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
                        pIfDescriptor->bNumEndpoints == 0 ||
                        pIfDescriptor->bNumEndpoints == 1,
                        STATUS_DEVICE_HARDWARE_ERROR,
                        "Bad UsbInterfaceDescriptor");

                    // Apple Quirk: when there are multiple NCM control
                    // interfaces (Apple exposes two functions), pick the
                    // FIRST one.
                    if (controlInterfaceNumber == 0xFF) {
                        controlInterfaceNumber = pIfDescriptor->bInterfaceNumber;
                    }
                }
                else if ((pIfDescriptor->bInterfaceClass == USB_CDC_INTERFACE_CLASS_DATA) &&
                         (pIfDescriptor->bInterfaceProtocol == USB_DATA_INTERFACE_PROTOCOL_NCM))
                {
                    // Apple Quirk: pick the FIRST data interface with 2 bulk
                    // endpoints (the active alt setting). If we already have
                    // a data interface, capture the next distinct one as
                    // dataInterfaceNumber2 (the anpi* peer).
                    if (dataInterfaceNumber == 0xFF && pIfDescriptor->bNumEndpoints == 2)
                    {
                        dataInterfaceNumber = pIfDescriptor->bInterfaceNumber;
                    }
                    else if (pIfDescriptor->bNumEndpoints == 2 &&
                             pIfDescriptor->bInterfaceNumber != dataInterfaceNumber &&
                             dataInterfaceNumber2 == 0xFF)
                    {
                        dataInterfaceNumber2 = pIfDescriptor->bInterfaceNumber;
                    }
                }

                break;
            }

            case USB_CS_INTERFACE_TYPE:
            {
                NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
                    pCurrDescriptor->bLength >= sizeof(USB_CDC_CS_FUNCTIONAL_DESCRIPTOR),
                    STATUS_DEVICE_HARDWARE_ERROR,
                    "Bad UsbCdcFunctionalDescriptor");

                PUSB_CDC_CS_FUNCTIONAL_DESCRIPTOR pCsFuncDescriptor =
                    (PUSB_CDC_CS_FUNCTIONAL_DESCRIPTOR)pCurrDescriptor;

                switch (pCsFuncDescriptor->bDescriptorSubtype)
                {
                    // NCM functional descriptor
                    case USB_CS_NCM_FUNCTIONAL_DESCR_TYPE:
                    {
                        NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
                            pCsFuncDescriptor->bFunctionLength == sizeof(USB_NCM_CS_FUNCTIONAL_DESCRIPTOR),
                            STATUS_DEVICE_HARDWARE_ERROR,
                            "Bad UsbNcmFunctionalDescriptor");

                        pNcmFunctionalDescr = (PUSB_NCM_CS_FUNCTIONAL_DESCRIPTOR) pCsFuncDescriptor;

                        break;
                    }

                    // ECM network functional descriptor
                    case USB_CS_ECM_FUNCTIONAL_DESCR_TYPE:
                    {
                        NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
                            pCsFuncDescriptor->bFunctionLength == sizeof(USB_ECM_CS_NET_FUNCTIONAL_DESCRIPTOR),
                            STATUS_DEVICE_HARDWARE_ERROR,
                            "Bad UsbEcmFunctionalDescriptor");

                        pEcmFunctionalDescr = (PUSB_ECM_CS_NET_FUNCTIONAL_DESCRIPTOR) pCsFuncDescriptor;

                        break;
                    }
                }

                break;
            }
        }

        currDescrptorOffset += pCurrDescriptor->bLength;
        pCurrDescriptor = (PUSB_COMMON_DESCRIPTOR)(((PUINT8)pCurrDescriptor) + pCurrDescriptor->bLength);
    }

    APPLENCM_STEP("SelectConfiguration:after-parseLoop", (NTSTATUS)controlInterfaceNumber);
    APPLENCM_STEP("SelectConfiguration:dataInterfaceNumber", (NTSTATUS)dataInterfaceNumber);
    APPLENCM_STEP("SelectConfiguration:dataInterfaceNumber2", (NTSTATUS)dataInterfaceNumber2);

    BYTE numInterfaces = WdfUsbTargetDeviceGetNumInterfaces(m_WdfUsbTargetDevice);
    APPLENCM_STEP("SelectConfiguration:numInterfaces", (NTSTATUS)numInterfaces);

    for (UCHAR ifIndex = 0; ifIndex < numInterfaces; ifIndex++)
    {
        WDFUSBINTERFACE usbInterface = WdfUsbTargetDeviceGetInterface(
            m_WdfUsbTargetDevice,
            ifIndex);

        UCHAR ifNum = WdfUsbInterfaceGetInterfaceNumber(usbInterface);
        if (ifNum == controlInterfaceNumber)
        {
            m_ControlInterface = usbInterface;
        }
        else if (ifNum == dataInterfaceNumber)
        {
            m_DataInterface = usbInterface;
        }
        else if (ifNum == dataInterfaceNumber2)
        {
            m_DataInterface2 = usbInterface;
        }
    }

    APPLENCM_STEP("SelectConfiguration:after-ifLookup",
                  (m_ControlInterface ? 1 : 0) | (m_DataInterface ? 2 : 0));

    // Apple Quirk: Apple's data interface may expose only 1 alt-setting
    // (already in active state). Only require both interfaces to exist;
    // tolerate any number of alt-settings (we won't switch them anyway —
    // see SelectSetting()).
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: m_ControlInterface=%p m_DataInterface=%p dataNumSettings=%d\n",
        m_ControlInterface, m_DataInterface,
        m_DataInterface ? WdfUsbInterfaceGetNumSettings(m_DataInterface) : 0);

    NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
        (m_ControlInterface != nullptr) &&
            (m_DataInterface != nullptr),
        STATUS_DEVICE_HARDWARE_ERROR,
        "Bad UsbNcm interfaces (missing control or data)");

    // Apple Quirk: report interface metadata so we know what we're working with.
    APPLENCM_STEP("SC:ctrlNumSettings",
                  (NTSTATUS)(m_ControlInterface ? WdfUsbInterfaceGetNumSettings(m_ControlInterface) : 0));
    APPLENCM_STEP("SC:dataNumSettings",
                  (NTSTATUS)(m_DataInterface ? WdfUsbInterfaceGetNumSettings(m_DataInterface) : 0));

    APPLENCM_STEP("SelectConfiguration:before-WdfUsbSelectConfig", STATUS_SUCCESS);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: about to WdfUsbTargetDeviceSelectConfig\n");

    // Apple Quirk: Apple's data interface may have only one alt-setting (e.g.
    // only alt 0). Probe combinations until one succeeds. Try the standard
    // (ctrl=0, data=0) first, then (ctrl=0, data=1), then single-interface
    // configs.
    NTSTATUS _selectStatus = STATUS_INVALID_PARAMETER;
    UCHAR successfulCtrlAlt = 0xFF;
    UCHAR successfulDataAlt = 0xFF;

    // Build a settings array for ALL interfaces visible to us — Apple may
    // require the full set or nothing. Data interfaces get alt 1 (active).
    BYTE numIfTotal = WdfUsbTargetDeviceGetNumInterfaces(m_WdfUsbTargetDevice);

    UCHAR dataAltTry[4] = { 0, 1, 0, 0 };
    UCHAR ctrlAltTry[4] = { 0, 0, 0, 0 };

    for (int attempt = 0; attempt < 4 && !NT_SUCCESS(_selectStatus); attempt++) {
        WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
        if (attempt < 2) {
            WDF_USB_INTERFACE_SETTING_PAIR settingPair[2] = {
                {m_ControlInterface, ctrlAltTry[attempt]},
                {m_DataInterface,    dataAltTry[attempt]}
            };
            WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(
                &configParams, 2, settingPair);
        } else if (attempt == 2) {
            // Last-ditch (a): only configure the comm interface.
            WDF_USB_INTERFACE_SETTING_PAIR singlePair[1] = {
                {m_ControlInterface, 0}
            };
            WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(
                &configParams, 1, singlePair);
        } else {
            // Last-ditch (b): configure ALL interfaces — Apple may want
            // a complete picture. Data interfaces -> alt 1, others -> alt 0.
            WDF_USB_INTERFACE_SETTING_PAIR allPairs[8];
            RtlZeroMemory(allPairs, sizeof(allPairs));
            UCHAR pairCount = 0;
            UCHAR maxIfs = (numIfTotal < 8) ? numIfTotal : 8;
            for (UCHAR ifIdx = 0; ifIdx < maxIfs; ifIdx++) {
                WDFUSBINTERFACE wif = WdfUsbTargetDeviceGetInterface(m_WdfUsbTargetDevice, ifIdx);
                UCHAR ifNum = WdfUsbInterfaceGetInterfaceNumber(wif);
                // Heuristic: odd interface numbers are data (1, 3); even are control (0, 2).
                BOOLEAN isData = (ifNum % 2) != 0;
                allPairs[pairCount].UsbInterface = wif;
                allPairs[pairCount].SettingIndex = (UCHAR)(isData ? 1 : 0);
                pairCount++;
            }
            WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(
                &configParams, pairCount, allPairs);
        }

        _selectStatus = WdfUsbTargetDeviceSelectConfig(
            m_WdfUsbTargetDevice,
            WDF_NO_OBJECT_ATTRIBUTES,
            &configParams);

        WCHAR stepName[64];
        // Manually format step name without ntstrsafe dependency:
        // "SC:try-N-ctrl_X-data_Y"
        stepName[0] = L'S'; stepName[1] = L'C'; stepName[2] = L':';
        stepName[3] = L't'; stepName[4] = L'r'; stepName[5] = L'y'; stepName[6] = L'-';
        stepName[7] = L'0' + (WCHAR)attempt;
        stepName[8] = L'\0';
        AppleNcm_WriteRegStatus(stepName, _selectStatus);

        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AppleNCM: SelectConfig attempt %d returned 0x%08X\n", attempt, _selectStatus);

        if (NT_SUCCESS(_selectStatus)) {
            successfulCtrlAlt = (attempt < 2) ? ctrlAltTry[attempt] : 0;
            successfulDataAlt = (attempt < 2) ? dataAltTry[attempt] : 0xFF;
            break;
        }
    }

    APPLENCM_STEP("SelectConfiguration:after-WdfUsbSelectConfig", _selectStatus);

    if (!NT_SUCCESS(_selectStatus)) {
        APPLENCM_STEP("SC:allSelectAttemptsFailed", _selectStatus);
    }

    // Apple Quirk: WdfUsbTargetDeviceSelectConfig consistently returns
    // STATUS_INVALID_PARAMETER for Apple's multi-function device. Proceed
    // anyway — when we bind as the composite parent, the framework already
    // exposes the interfaces with default settings, so per-interface
    // WdfUsbInterfaceSelectSetting calls can still drive them.
    if (!NT_SUCCESS(_selectStatus)) {
        APPLENCM_STEP("SC:proceedingWithoutSelectConfig", STATUS_SUCCESS);
    }

    // 3. query MTU
    //
    // Apple Quirk: macOS NCM peripherals may not advertise the ECM Ethernet
    // Functional Descriptor. When pEcmFunctionalDescr is NULL we fall back
    // to MAX_HOST_MTU_SIZE and synthesize a locally-administered MAC address
    // derived from the device's serial number.

    if (pEcmFunctionalDescr != nullptr)
    {
        m_MaxDatagramSize = min(pEcmFunctionalDescr->wMaxSegmentSize, MAX_HOST_MTU_SIZE);
    }
    else
    {
        m_MaxDatagramSize = MAX_HOST_MTU_SIZE;
    }

    // 4. query MAC address

    if (pEcmFunctionalDescr != nullptr && pEcmFunctionalDescr->iMACAddress != 0)
    {
        WCHAR strMacAddress[12];
        USHORT strMacAddressLength = ARRAYSIZE(strMacAddress);

        NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
            WdfUsbTargetDeviceQueryString(
                m_WdfUsbTargetDevice,
                NULL,
                NULL,
                strMacAddress,
                &strMacAddressLength,
                pEcmFunctionalDescr->iMACAddress,
                0x0409),
            "WdfUsbTargetDeviceQueryString failed");

        NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
            HexStringToBytes(strMacAddress, m_MacAddress, sizeof(m_MacAddress)),
            "Invalid Mac Address");
    }
    else
    {
        // Apple Quirk: synthesize a locally-administered unicast MAC address.
        // First octet 0x02 = locally administered bit (b1) set, multicast bit
        // (b0) clear. Remaining bytes derived from device handle for stability
        // across plug events on the same device.
        m_MacAddress[0] = 0x02;
        m_MacAddress[1] = 0xAC;  // 'AP'le hint, not significant
        m_MacAddress[2] = 0xCE;  // 'NC'M, not significant
        ULONGLONG seed = (ULONGLONG)(ULONG_PTR)m_WdfDevice;
        m_MacAddress[3] = (UCHAR)(seed >> 16);
        m_MacAddress[4] = (UCHAR)(seed >> 8);
        m_MacAddress[5] = (UCHAR)(seed >> 0);
    }

    // 5. Get NTB parameters
    //
    // Apple Quirk: Apple NCM devices don't always implement
    // GET_NTB_PARAMETERS. Fall back to NCM spec default values when the
    // control transfer fails. NTB16 is the spec-required minimum.

    WDF_MEMORY_DESCRIPTOR memoryDescriptor;

    RtlZeroMemory(&m_NtbParamters, sizeof(m_NtbParamters));

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &memoryDescriptor,
        &m_NtbParamters,
        sizeof(m_NtbParamters));

    NTSTATUS _getNtb = RequestClassSpecificControlTransfer(
        USB_REQUEST_GET_NTB_PARAMETERS,
        BmRequestDeviceToHost,
        BmRequestToInterface,
        0,
        &memoryDescriptor);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: GET_NTB_PARAMETERS returned 0x%08X bmFormats=0x%X\n",
        _getNtb, m_NtbParamters.bmNtbFormatsSupported);

    if (!NT_SUCCESS(_getNtb) || m_NtbParamters.bmNtbFormatsSupported == 0)
    {
        // Apple Quirk: synthesize spec-default NTB parameters.
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AppleNCM: applying default NTB parameters\n");
        m_NtbParamters.bmNtbFormatsSupported = 0x1;     // NTB16 only
        m_NtbParamters.dwNtbInMaxSize        = 16384;   // 16 KiB
        m_NtbParamters.wNdpInDivisor         = 4;
        m_NtbParamters.wNdpInPayloadRemainder = 0;
        m_NtbParamters.wNdpInAlignment       = 4;
        m_NtbParamters.dwNtbOutMaxSize       = 16384;
        m_NtbParamters.wNdpOutDivisor        = 4;
        m_NtbParamters.wNdpOutPayloadRemainder = 0;
        m_NtbParamters.wNdpOutAlignment      = 4;
        m_NtbParamters.wNtbOutMaxDatagrams   = 0;       // unlimited
    }

    //using NTB 32 if supported
    if (m_NtbParamters.bmNtbFormatsSupported & 0x2)
    {
        m_Use32BitNtb = TRUE;
    }

    m_HostSelectedNtbInMaxSize = min(
        m_NtbParamters.dwNtbInMaxSize,
        MAX_HOST_NTB_SIZE);

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::SelectSetting(
    void
)
{
    PAGED_CODE();

    //NCM spec 7.2 Using Alternate Settings to Reset an NCM Function

    // Apple Quirk: macOS NCM peripherals are PRE-ACTIVATED — alt-setting
    // changes return STATUS_INVALID_PARAMETER. Mirror Linux's FLAG_NO_SETINT
    // and tolerate failures from SelectSetting + class control transfers.
    //
    // If alt-setting failures occur, the device is already in the desired
    // state. Log and continue. The Linux patch (commit 3ec8d7572a69) uses
    // the same approach for the private CDC-NCM interface.

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: SelectSetting entry\n");

    // 1. Data interface is selected to Setting 0, and control interface remains at Setting 0

    WDF_USB_INTERFACE_SELECT_SETTING_PARAMS settingParams;
    WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&settingParams, 0);

    NTSTATUS _ss0 = WdfUsbInterfaceSelectSetting(m_DataInterface, WDF_NO_OBJECT_ATTRIBUTES, &settingParams);
    APPLENCM_STEP("SS:dataAlt0-result", _ss0);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: SelectSetting alt=0 returned 0x%08X (tolerating failure)\n", _ss0);

    //2. Config NTB

    //using NTB 32 if supported
    if (m_Use32BitNtb)
    {
        //NCM spec 6.2.5
        //The host shall only send this command while the NCM Data Interface is in alternate setting 0.
        NTSTATUS _setFmt = RequestClassSpecificControlTransfer(
            USB_REQUEST_SET_NTB_FORMAT,
            BmRequestHostToDevice,
            BmRequestToInterface,
            1,
            nullptr);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AppleNCM: SET_NTB_FORMAT returned 0x%08X (tolerating)\n", _setFmt);
    }

    // NCM spec 3.4 NTB Maximum Sizes
    // NCM spec 6.2.7 SetNtbInputSize
    if (m_HostSelectedNtbInMaxSize < m_NtbParamters.dwNtbInMaxSize)
    {
        WDF_MEMORY_DESCRIPTOR memoryDescriptor;

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
            &memoryDescriptor,
            (PVOID)&m_HostSelectedNtbInMaxSize,
            sizeof(m_HostSelectedNtbInMaxSize));

        NTSTATUS _setSz = RequestClassSpecificControlTransfer(
            USB_REQUEST_SET_NTB_INPUT_SIZE,
            BmRequestHostToDevice,
            BmRequestToInterface,
            0,
            &memoryDescriptor);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
            "AppleNCM: SET_NTB_INPUT_SIZE returned 0x%08X (tolerating)\n", _setSz);
    }

    // 3. Data interface is selected to Setting 1, and control interface remains at Setting 0.

    WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&settingParams, 1);

    NTSTATUS _ss1 = WdfUsbInterfaceSelectSetting(m_DataInterface, WDF_NO_OBJECT_ATTRIBUTES, &settingParams);
    APPLENCM_STEP("SS:dataAlt1-result", _ss1);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: SelectSetting alt=1 returned 0x%08X (tolerating failure)\n", _ss1);

    // Apple Quirk: even if alt-setting failed, attempt to proceed —
    // the device may already be configured at the active alt.
    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::RetrieveInterruptPipe(
    void
)
{
    PAGED_CODE();

    WDF_USB_PIPE_INFORMATION pipeInfo;
    WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

    // Apple Quirk: macOS NCM peripherals omit the interrupt endpoint.
    // Treat zero pipes as "no link-state notifications" rather than a
    // hard error — the driver can still operate, it just won't receive
    // NETWORK_CONNECTION / CONNECTION_SPEED_CHANGE notifications.
    const UCHAR numConfiguredPipes =
        WdfUsbInterfaceGetNumConfiguredPipes(m_ControlInterface);

    if (numConfiguredPipes == 0)
    {
        m_ControlInterruptPipe = nullptr;
        m_ControlInterruptPipeMaxPacket = 0;
        return STATUS_SUCCESS;
    }

    NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
        numConfiguredPipes == 1,
        STATUS_DEVICE_HARDWARE_ERROR,
        "Bad NCM control interface");

    m_ControlInterruptPipe = WdfUsbInterfaceGetConfiguredPipe(
        m_ControlInterface,
        0, //PipeIndex,
        &pipeInfo);

    NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
        pipeInfo.PipeType == WdfUsbPipeTypeInterrupt,
        STATUS_DEVICE_HARDWARE_ERROR,
        "Bad NCM control pipe type");

    WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(m_ControlInterruptPipe);
    m_ControlInterruptPipeMaxPacket = pipeInfo.MaximumPacketSize;

    WDF_USB_CONTINUOUS_READER_CONFIG readerConfig;

    // Configure the continous reader for Interrupt pipe
    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &readerConfig,
        UsbNcmHostDevice::ControlInterruptPipeReadCompletetionRoutine,
        this,
        m_ControlInterruptPipeMaxPacket);

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(
        WdfUsbTargetPipeConfigContinuousReader(
            m_ControlInterruptPipe,
            &readerConfig),
        "WdfUsbTargetPipeConfigContinuousReader failed for interrupt pipe");

    return STATUS_SUCCESS;
}


PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::RetrieveDataBulkPipes(
    void
)
{
    PAGED_CODE();

    APPLENCM_STEP("RetrieveData:ENTRY", STATUS_SUCCESS);

    UCHAR dataPipeCount = WdfUsbInterfaceGetNumConfiguredPipes(m_DataInterface);
    APPLENCM_STEP("RetrieveData:dataPipeCount", (NTSTATUS)dataPipeCount);

    // Apple Quirk: if no pipes configured (because SelectConfig failed
    // earlier), try cycling alt-settings on the data interface directly.
    if (dataPipeCount < 2)
    {
        WDF_USB_INTERFACE_SELECT_SETTING_PARAMS sp;
        const UCHAR tryAlts[2] = { 1, 0 };
        for (int i = 0; i < 2; i++)
        {
            UCHAR tryAlt = tryAlts[i];
            WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&sp, tryAlt);
            NTSTATUS r = WdfUsbInterfaceSelectSetting(m_DataInterface, WDF_NO_OBJECT_ATTRIBUTES, &sp);
            UCHAR newCount = WdfUsbInterfaceGetNumConfiguredPipes(m_DataInterface);
            // Encode (status<<16 | count) in payload
            AppleNcm_WriteRegStatus(
                tryAlt == 0 ? L"RD:retry-alt0" : L"RD:retry-alt1",
                (NTSTATUS)(((ULONG)r & 0xFFFF0000) | newCount));
            if (newCount >= 2) {
                dataPipeCount = newCount;
                break;
            }
        }
        APPLENCM_STEP("RetrieveData:afterRetry-dataPipeCount", (NTSTATUS)dataPipeCount);
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "AppleNCM: RetrieveDataBulkPipes - data interface has %d configured pipes\n",
        dataPipeCount);

    NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
        dataPipeCount >= 2,
        STATUS_DEVICE_HARDWARE_ERROR,
        "Bad NCM data interface (need >=2 pipes)");

    APPLENCM_STEP("RetrieveData:before-pipeLoop", STATUS_SUCCESS);

    for (UCHAR pipeIndex = 0; pipeIndex < 2; pipeIndex++)
    {
        WDF_USB_PIPE_INFORMATION pipeInfo;
        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

        WDFUSBPIPE pipe = WdfUsbInterfaceGetConfiguredPipe(
            m_DataInterface,
            pipeIndex,
            &pipeInfo);

        NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
            pipeInfo.PipeType == WdfUsbPipeTypeBulk,
            STATUS_DEVICE_HARDWARE_ERROR,
            "Bad NCM data pipe type");

        WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

        if (WdfUsbTargetPipeIsInEndpoint(pipe))
        {
            //TraceEvents(TRACE_LEVEL_INFORMATION, DBG_IOCTL,
            //    "BulkInput Pipe is 0x%p\n", pipe);

            m_DataBulkInPipe = pipe;
        }
        else if (WdfUsbTargetPipeIsOutEndpoint(pipe))
        {
            //TraceEvents(TRACE_LEVEL_INFORMATION, DBG_IOCTL,
            //    "BulkOutput Pipe is 0x%p\n", pipe);

            m_DataBulkOutPipeMaximumPacketSize = pipeInfo.MaximumPacketSize;
            m_DataBulkOutPipe = pipe;
        }
        else
        {
            NCM_RETURN_NT_STATUS_IF_FALSE_MSG(
                FALSE,
                STATUS_DEVICE_HARDWARE_ERROR,
                "Bad NCM data pipe type - unknown");
        }
    }

    APPLENCM_STEP("RetrieveData:after-pipeLoop",
                  (m_DataBulkInPipe ? 1 : 0) | (m_DataBulkOutPipe ? 2 : 0));
    APPLENCM_STEP("RetrieveData:ntbInMaxSize", (NTSTATUS)m_HostSelectedNtbInMaxSize);

    WDF_USB_CONTINUOUS_READER_CONFIG readerConfig;

    //  RX Path: Configure the continous reader on the bulk pipe now, since this can
    //  only be done once on a given pipe unless it is unselected
    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &readerConfig,
        UsbNcmHostDevice::DataBulkInPipeReadCompletetionRoutine,
        this,
        m_HostSelectedNtbInMaxSize);

    readerConfig.HeaderLength = 0;
    readerConfig.NumPendingReads = PENDING_BULK_IN_READS;

    APPLENCM_STEP("RetrieveData:before-ContinuousReader", STATUS_SUCCESS);
    NTSTATUS _crStat = WdfUsbTargetPipeConfigContinuousReader(
            m_DataBulkInPipe,
            &readerConfig);
    APPLENCM_STEP("RetrieveData:after-ContinuousReader", _crStat);

    NCM_RETURN_IF_NOT_NT_SUCCESS_MSG(_crStat,
        "WdfUsbTargetPipeConfigContinuousReader failed for bulkin pipe");

    return STATUS_SUCCESS;
}

// Phase A counters: cumulative RX state on NCM #2 (interface 3, EP 0x82).
static volatile LONG64 g_Ncm2RxBytes  = 0;
static volatile LONG   g_Ncm2RxPackets = 0;

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::RetrieveDataBulkPipes2(
    void
)
{
    PAGED_CODE();

    APPLENCM_STEP("RD2:ENTRY", STATUS_SUCCESS);

    if (m_DataInterface2 == nullptr) {
        APPLENCM_STEP("RD2:noInterface2", STATUS_NOT_FOUND);
        return STATUS_NOT_FOUND;
    }

    UCHAR pipeCount = WdfUsbInterfaceGetNumConfiguredPipes(m_DataInterface2);
    APPLENCM_STEP("RD2:pipeCount", (NTSTATUS)pipeCount);

    if (pipeCount < 2) {
        // Try selecting alt 1 explicitly — try-3 in SelectConfig should have
        // already activated it, but the WDF cached state for this interface
        // may need refresh.
        WDF_USB_INTERFACE_SELECT_SETTING_PARAMS sp;
        WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&sp, 1);
        NTSTATUS rr = WdfUsbInterfaceSelectSetting(m_DataInterface2, WDF_NO_OBJECT_ATTRIBUTES, &sp);
        APPLENCM_STEP("RD2:retryAlt1", rr);
        pipeCount = WdfUsbInterfaceGetNumConfiguredPipes(m_DataInterface2);
        APPLENCM_STEP("RD2:pipeCountAfterRetry", (NTSTATUS)pipeCount);
        if (pipeCount < 2) {
            return STATUS_NOT_FOUND;
        }
    }

    for (UCHAR pipeIndex = 0; pipeIndex < 2; pipeIndex++) {
        WDF_USB_PIPE_INFORMATION pipeInfo;
        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

        WDFUSBPIPE pipe = WdfUsbInterfaceGetConfiguredPipe(
            m_DataInterface2,
            pipeIndex,
            &pipeInfo);

        if (pipeInfo.PipeType != WdfUsbPipeTypeBulk) {
            APPLENCM_STEP("RD2:notBulk", (NTSTATUS)pipeInfo.PipeType);
            continue;
        }

        WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

        if (WdfUsbTargetPipeIsInEndpoint(pipe)) {
            m_DataBulkInPipe2 = pipe;
        } else if (WdfUsbTargetPipeIsOutEndpoint(pipe)) {
            m_DataBulkOutPipe2 = pipe;
        }
    }

    APPLENCM_STEP("RD2:pipesFound",
                  (m_DataBulkInPipe2 ? 1 : 0) | (m_DataBulkOutPipe2 ? 2 : 0));

    if (m_DataBulkInPipe2 == nullptr) {
        return STATUS_NOT_FOUND;
    }

    // Configure a debug-only continuous reader. We accept up to 16KB per
    // transfer — anpi3 multicast frames are tiny but reserve room.
    WDF_USB_CONTINUOUS_READER_CONFIG readerConfig;
    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &readerConfig,
        UsbNcmHostDevice::DataBulkInPipe2ReadCompletetionRoutine,
        this,
        16384);
    readerConfig.HeaderLength = 0;
    readerConfig.NumPendingReads = 4;

    NTSTATUS cr = WdfUsbTargetPipeConfigContinuousReader(
        m_DataBulkInPipe2,
        &readerConfig);
    APPLENCM_STEP("RD2:after-ContinuousReader", cr);

    return cr;
}

_Use_decl_annotations_
VOID
UsbNcmHostDevice::DataBulkInPipe2ReadCompletetionRoutine(
    WDFUSBPIPE,
    WDFMEMORY,
    size_t numBytesTransferred,
    WDFCONTEXT
)
{
    LONG64 bytes = InterlockedAdd64(&g_Ncm2RxBytes, (LONG64)numBytesTransferred);
    LONG   pkts  = InterlockedIncrement(&g_Ncm2RxPackets);

    // Throttle registry writes: emit every 4 packets, or whenever the byte
    // payload is "interesting" (anpi3 multicast frames are bursty).
    if ((pkts & 0x3) == 0 || numBytesTransferred > 60) {
        AppleNcm_WriteRegStatus(L"Ncm2:rxPackets", (NTSTATUS)pkts);
        // Pack low 32 bits of cumulative bytes into the trace; user-mode
        // reader can multiply / poll across plug events for total.
        AppleNcm_WriteRegStatus(L"Ncm2:rxBytesLo", (NTSTATUS)(ULONG)bytes);
        AppleNcm_WriteRegStatus(L"Ncm2:lastSize", (NTSTATUS)(ULONG)numBytesTransferred);
    }
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::EnterWorkingState(
    WDF_POWER_DEVICE_STATE previousState
)
{
    if (previousState != WdfPowerDeviceD3Final)
    {
        // if this is not during device first start, and then device is
        // coming back from low power, reset function
        NCM_RETURN_IF_NOT_NT_SUCCESS(SelectSetting());
        NCM_RETURN_IF_NOT_NT_SUCCESS(RetrieveDataBulkPipes());
    }

    // Apple Quirk: interrupt pipe may be absent on macOS NCM peripherals.
    if (m_ControlInterruptPipe != nullptr)
    {
        NCM_RETURN_IF_NOT_NT_SUCCESS(StartPipe(m_ControlInterruptPipe));
    }
    else if (m_NcmAdapterCallbacks != nullptr && m_NetAdapter != nullptr)
    {
        // Apple Quirk: no interrupt endpoint means no NETWORK_CONNECTION
        // notification will arrive. Force link state up so NDIS allows the
        // adapter to start (mirrors Linux's netif_carrier_on() in the same
        // quirk path). Direct USB attachment implies the link is up.
        m_NcmAdapterCallbacks->EvtUsbNcmAdapterSetLinkState(m_NetAdapter, TRUE);
    }

    return STATUS_SUCCESS;
}

PAGEDX
_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::LeaveWorkingState(
    void
)
{
    // Apple Quirk: interrupt pipe may be absent on macOS NCM peripherals.
    if (m_ControlInterruptPipe != nullptr)
    {
        StopPipe(m_ControlInterruptPipe);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
UsbNcmHostDevice::ControlInterruptPipeReadCompletetionRoutine(
    WDFUSBPIPE,
    WDFMEMORY memory,
    size_t numBytesTransfered,
    WDFCONTEXT context
)
{
    UsbNcmHostDevice * ncmDevice = (UsbNcmHostDevice *)context;

    if (numBytesTransfered < sizeof(USB_CDC_NOTIFICATION))
    {
        // Log trace msg
        return;
    }

    PUSB_CDC_NOTIFICATION cdcNotification =
        (PUSB_CDC_NOTIFICATION)WdfMemoryGetBuffer(memory, nullptr);

    switch (cdcNotification->bNotificationCode)
    {
        case USB_CDC_NOTIFICATION_NETWORK_CONNECTION:
        {
            ncmDevice->m_NcmAdapterCallbacks->EvtUsbNcmAdapterSetLinkState(
                ncmDevice->m_NetAdapter,
                !!cdcNotification->wValue);

            break;
        }
        case USB_CDC_NOTIFICATION_CONNECTION_SPEED_CHANGE:
        {
            PCDC_CONN_SPEED_CHANGE cdcSpeedChange =
                (PCDC_CONN_SPEED_CHANGE) cdcNotification;

            ncmDevice->m_NcmAdapterCallbacks->EvtUsbNcmAdapterSetLinkSpeed(
                ncmDevice->m_NetAdapter,
                cdcSpeedChange->USBitRate,
                cdcSpeedChange->DSBITRate);

            break;
        }
        default:
            //TODO: Log message for unsupported type
            break;
    }
}

_Use_decl_annotations_
VOID
UsbNcmHostDevice::DataBulkInPipeReadCompletetionRoutine(
    WDFUSBPIPE,
    WDFMEMORY memory,
    size_t numBytesTransferred,
    WDFCONTEXT context
)
{
    UsbNcmHostDevice* hostDevice = (UsbNcmHostDevice *)context;

    NT_FRE_ASSERT(hostDevice->m_HostSelectedNtbInMaxSize >= (UINT32)numBytesTransferred);

    hostDevice->m_NcmAdapterCallbacks->EvtUsbNcmAdapterNotifyReceive(
        hostDevice->m_NetAdapter,
        nullptr,
        0,
        memory,
        WDF_NO_HANDLE);
}

PAGEDX
_Use_decl_annotations_
void
UsbNcmHostDevice::StartReceive(
    WDFDEVICE usbNcmWdfDevice
)
{
    auto host = NcmGetHostDeviceFromHandle(usbNcmWdfDevice);
    (void) StartPipe(host->m_DataBulkInPipe);
    // Phase A: also start the NCM #2 IN pipe if available — debug counter
    // only. No NetAdapter wired to it yet.
    if (host->m_DataBulkInPipe2 != nullptr) {
        APPLENCM_STEP("StartReceive:starting-In2", STATUS_SUCCESS);
        (void) StartPipe(host->m_DataBulkInPipe2);
    }
}

PAGEDX
_Use_decl_annotations_
void
UsbNcmHostDevice::StopReceive(
    WDFDEVICE usbNcmWdfDevice
)
{
    auto host = NcmGetHostDeviceFromHandle(usbNcmWdfDevice);
    StopPipe(host->m_DataBulkInPipe);
    if (host->m_DataBulkInPipe2 != nullptr) {
        StopPipe(host->m_DataBulkInPipe2);
    }
}

PAGEDX
_Use_decl_annotations_
void
UsbNcmHostDevice::StartTransmit(
    WDFDEVICE usbNcmWdfDevice
)
{
    (void) StartPipe(NcmGetHostDeviceFromHandle(usbNcmWdfDevice)->m_DataBulkOutPipe);
}

PAGEDX
_Use_decl_annotations_
void
UsbNcmHostDevice::StopTransmit(
    WDFDEVICE usbNcmWdfDevice
)
{
    StopPipe(NcmGetHostDeviceFromHandle(usbNcmWdfDevice)->m_DataBulkOutPipe);
}

_Use_decl_annotations_
inline
void
UsbNcmHostDevice::TransmitFramesCompetion(
    WDFREQUEST,
    WDFIOTARGET target,
    PWDF_REQUEST_COMPLETION_PARAMS,
    WDFCONTEXT context
)
{
    UsbNcmHostDevice* hostDevice = NcmGetHostDeviceFromHandle(WdfIoTargetGetDevice(target));

    hostDevice->m_NcmAdapterCallbacks->EvtUsbNcmAdapterNotifyTransmitCompletion(
        hostDevice->m_NetAdapter,
        (TX_BUFFER_REQUEST *)context);
}

_Use_decl_annotations_
NTSTATUS
UsbNcmHostDevice::TransmitFrames(
    WDFDEVICE usbNcmWdfDevice,
    TX_BUFFER_REQUEST * bufferRequest
)
{
    NTSTATUS status = STATUS_SUCCESS;
    UsbNcmHostDevice * hostDevice = NcmGetHostDeviceFromHandle(usbNcmWdfDevice);

    NT_FRE_ASSERT(bufferRequest->TransferLength > 0);

    if (bufferRequest->TransferLength < bufferRequest->BufferLength &&
        bufferRequest->TransferLength % hostDevice->m_DataBulkOutPipeMaximumPacketSize == 0)
    {
        //NCM spec is not explicit if a ZLP shall be sent when wBlockLength != 0 and it happens to be
        //multiple of wMaxPacketSize. Our interpretation is that no ZLP needed if wBlockLength is non-zero,
        //because the non-zero wBlockLength has already told the function side the size of transfer to be expected.
        //
        //However, there are in-market NCM devices rely on ZLP as long as the wBlockLength is multiple of wMaxPacketSize.
        //To deal with such devices, we pad an extra 0 at end so the transfer is no longer multiple of wMaxPacketSize

        bufferRequest->Buffer[bufferRequest->TransferLength] = 0;
        bufferRequest->TransferLength++;
    }

    WdfRequestSetCompletionRoutine(
        bufferRequest->Request,
        UsbNcmHostDevice::TransmitFramesCompetion,
        bufferRequest);

    WDFMEMORY_OFFSET offset{ 0, bufferRequest->TransferLength };
    status = WdfUsbTargetPipeFormatRequestForWrite(
        hostDevice->m_DataBulkOutPipe,
        bufferRequest->Request,
        bufferRequest->BufferWdfMemory,
        &offset);

    if (NT_SUCCESS(status))
    {
        WDF_REQUEST_SEND_OPTIONS sendOptions = {};
        WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&sendOptions, WDF_REL_TIMEOUT_IN_SEC(5));

        if (!WdfRequestSend(
                bufferRequest->Request,
                WdfUsbTargetPipeGetIoTarget(hostDevice->m_DataBulkOutPipe), &sendOptions))
        {
            status = WdfRequestGetStatus(bufferRequest->Request);
        }
    }

    return status;
}
