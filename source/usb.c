/*
 * usb.c
 *
 * Heavily based in usb_comms from libnx.
 *
 * Copyright (c) 2018-2020, Switchbrew and libnx contributors.
 * Copyright (c) 2020, DarkMatterCore <pabloacurielz@gmail.com>.
 *
 * This file is part of nxdumptool (https://github.com/DarkMatterCore/nxdumptool).
 *
 * nxdumptool is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * nxdumptool is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "utils.h"
#include "usb.h"

#define USB_ABI_VERSION             1

#define USB_CMD_HEADER_MAGIC        0x4E584454          /* "NXDT". */

#define USB_TRANSFER_ALIGNMENT      0x1000              /* 4 KiB. */
#define USB_TRANSFER_TIMEOUT        1                   /* 1 second. */

/* Type definitions. */

typedef struct {
    RwLock lock, lock_in, lock_out;
    bool initialized;
    UsbDsInterface *interface;
    UsbDsEndpoint *endpoint_in, *endpoint_out;
} usbDeviceInterface;

typedef enum {
    UsbCommandType_StartSession       = 0,
    UsbCommandType_SendFileProperties = 1,
    UsbCommandType_SendNspHeader      = 2,  ///< Needs to be implemented.
    UsbCommandType_EndSession         = 3
} UsbCommandType;

typedef struct {
    u32 magic;
    u32 cmd;
    u32 cmd_block_size;
    u8 reserved[0x4];
} UsbCommandHeader;

typedef struct {
    u8 app_ver_major;
    u8 app_ver_minor;
    u8 app_ver_micro;
    u8 abi_version;
    u8 reserved[0xC];
} UsbCommandStartSession;

typedef struct {
    u64 file_size;
    u32 filename_length;
    u8 reserved_1[0x4];
    char filename[FS_MAX_PATH];
    u8 reserved_2[0xF];
} UsbCommandSendFileProperties;

typedef enum {
    ///< Expected response code.
    UsbStatusType_Success               = 0,
    
    ///< Internal usage.
    UsbStatusType_InvalidCommandSize    = 1,
    UsbStatusType_WriteCommandFailed    = 2,
    UsbStatusType_ReadStatusFailed      = 3,
    
    ///< These can be returned by the host device.
    UsbStatusType_InvalidMagicWord      = 4,
    UsbStatusType_UnsupportedCommand    = 5,
    UsbStatusType_UnsupportedAbiVersion = 6,
    UsbStatusType_MalformedCommand      = 7,
    UsbStatusType_HostIoError           = 8
} UsbStatusType;

typedef struct {
    u32 magic;
    u32 status;
    u8 reserved[0x8];
} UsbStatus;

/* Global variables. */

static RwLock g_usbDeviceLock = {0};
static usbDeviceInterface g_usbDeviceInterface = {0};
static bool g_usbDeviceInterfaceInitialized = false;

static Event *g_usbStateChangeEvent = NULL;
static thrd_t g_usbDetectionThread;
static UEvent g_usbDetectionThreadExitEvent = {0}, g_usbTimeoutEvent = {0};
static bool g_usbHostAvailable = false, g_usbSessionStarted = false, g_usbDetectionThreadExitFlag = false;
static atomic_bool g_usbDetectionThreadCreated = false;

static u8 *g_usbTransferBuffer = NULL;
static u64 g_usbTransferRemainingSize = 0;

/* Function prototypes. */

static bool usbCreateDetectionThread(void);
static void usbDestroyDetectionThread(void);
static int usbDetectionThreadFunc(void *arg);

static bool usbStartSession(void);
static void usbEndSession(void);

NX_INLINE void usbPrepareCommandHeader(u32 cmd, u32 cmd_block_size);
static u32 usbSendCommand(size_t cmd_size);
NX_INLINE void usbLogStatusDetail(u32 status);

NX_INLINE bool usbAllocateTransferBuffer(void);
NX_INLINE void usbFreeTransferBuffer(void);

static bool usbInitializeComms(void);
static void usbCloseComms(void);

static void usbFreeDeviceInterface(void);

NX_INLINE bool usbInitializeDeviceInterface(void);
static bool usbInitializeDeviceInterface5x(void);
static bool usbInitializeDeviceInterface1x(void);

NX_INLINE bool usbIsHostAvailable(void);

NX_INLINE bool usbRead(void *buf, size_t size);
NX_INLINE bool usbWrite(void *buf, size_t size);
static bool usbTransferData(void *buf, size_t size, UsbDsEndpoint *endpoint);

bool usbInitialize(void)
{
    bool ret = false;
    
    rwlockWriteLock(&g_usbDeviceLock);
    
    /* Allocate USB transfer buffer. */
    if (!usbAllocateTransferBuffer())
    {
        LOGFILE("Failed to allocate memory for the USB transfer buffer!");
        goto end;
    }
    
    /* Initialize USB device interface. */
    if (!usbInitializeComms())
    {
        LOGFILE("Failed to initialize USB device interface!");
        goto end;
    }
    
    /* Retrieve USB state change kernel event. */
    g_usbStateChangeEvent = usbDsGetStateChangeEvent();
    if (!g_usbStateChangeEvent)
    {
        LOGFILE("Failed to retrieve USB state change kernel event!");
        goto end;
    }
    
    /* Create usermode exit event. */
    ueventCreate(&g_usbDetectionThreadExitEvent, true);
    
    /* Create usermode USB timeout event. */
    ueventCreate(&g_usbTimeoutEvent, true);
    
    /* Create USB detection thread. */
    atomic_store(&g_usbDetectionThreadCreated, usbCreateDetectionThread());
    if (!atomic_load(&g_usbDetectionThreadCreated)) goto end;
    
    ret = true;
    
end:
    rwlockWriteUnlock(&g_usbDeviceLock);
    
    return ret;
}

void usbExit(void)
{
    /* Destroy USB detection thread before attempting to lock. */
    if (atomic_load(&g_usbDetectionThreadCreated))
    {
        usbDestroyDetectionThread();
        atomic_store(&g_usbDetectionThreadCreated, false);
    }
    
    /* Now we can safely lock. */
    rwlockWriteLock(&g_usbDeviceLock);
    
    /* Clear USB state change kernel event. */
    g_usbStateChangeEvent = NULL;
    
    /* Close USB device interface. */
    usbCloseComms();
    
    /* Free USB transfer buffer. */
    usbFreeTransferBuffer();
    
    rwlockWriteUnlock(&g_usbDeviceLock);
}

void *usbAllocatePageAlignedBuffer(size_t size)
{
    if (!size) return NULL;
    return memalign(USB_TRANSFER_ALIGNMENT, size);
}

bool usbIsReady(void)
{
    rwlockWriteLock(&g_usbDeviceLock);
    rwlockWriteLock(&(g_usbDeviceInterface.lock));
    bool ret = (g_usbHostAvailable && g_usbSessionStarted);
    rwlockWriteUnlock(&(g_usbDeviceInterface.lock));
    rwlockWriteUnlock(&g_usbDeviceLock);
    return ret;
}

bool usbSendFileProperties(u64 file_size, const char *filename)
{
    rwlockWriteLock(&g_usbDeviceLock);
    rwlockWriteLock(&(g_usbDeviceInterface.lock));
    
    bool ret = false;
    UsbCommandSendFileProperties *cmd_block = NULL;
    size_t cmd_size = 0;
    u32 status = UsbStatusType_Success;
    u32 filename_length = 0;
    
    if (!g_usbTransferBuffer || !g_usbDeviceInterfaceInitialized || !g_usbDeviceInterface.initialized || !g_usbHostAvailable || !g_usbSessionStarted || g_usbTransferRemainingSize > 0 || !filename || \
        !(filename_length = (u32)strlen(filename)) || filename_length >= FS_MAX_PATH)
    {
        LOGFILE("Invalid parameters!");
        goto end;
    }
    
    usbPrepareCommandHeader(UsbCommandType_SendFileProperties, (u32)sizeof(UsbCommandSendFileProperties));
    
    cmd_block = (UsbCommandSendFileProperties*)(g_usbTransferBuffer + sizeof(UsbCommandHeader));
    memset(cmd_block, 0, sizeof(UsbCommandSendFileProperties));
    
    cmd_block->file_size = file_size;
    cmd_block->filename_length = filename_length;
    sprintf(cmd_block->filename, "%s", filename);
    
    cmd_size = (sizeof(UsbCommandHeader) + sizeof(UsbCommandSendFileProperties));
    
    status = usbSendCommand(cmd_size);
    if (status == UsbStatusType_Success)
    {
        ret = true;
        g_usbTransferRemainingSize = file_size;
    } else {
        usbLogStatusDetail(status);
    }
    
end:
    rwlockWriteUnlock(&(g_usbDeviceInterface.lock));
    rwlockWriteUnlock(&g_usbDeviceLock);
    
    return ret;
}

bool usbSendFileData(void *data, u64 data_size)
{
    rwlockWriteLock(&g_usbDeviceLock);
    rwlockWriteLock(&(g_usbDeviceInterface.lock));
    
    bool ret = false;
    void *buf = NULL;
    UsbStatus *cmd_status = NULL;
    
    if (!g_usbTransferBuffer || !g_usbDeviceInterfaceInitialized || !g_usbDeviceInterface.initialized || !g_usbHostAvailable || !g_usbSessionStarted || !g_usbTransferRemainingSize || !data || \
        !data_size || data_size > USB_TRANSFER_BUFFER_SIZE || data_size > g_usbTransferRemainingSize)
    {
        LOGFILE("Invalid parameters!");
        goto end;
    }
    
    /* Optimization for buffers that already are page aligned. */
    if (IS_ALIGNED((u64)data, USB_TRANSFER_ALIGNMENT))
    {
        buf = data;
    } else {
        buf = g_usbTransferBuffer;
        memcpy(buf, data, data_size);
    }
    
    if (!usbWrite(buf, data_size))
    {
        LOGFILE("Failed to write 0x%lX bytes long file data chunk!", data_size);
        goto end;
    }
    
    ret = true;
    g_usbTransferRemainingSize -= data_size;
    
    /* Check if this is the last chunk. */
    if (!g_usbTransferRemainingSize)
    {
        if (!usbRead(g_usbTransferBuffer, sizeof(UsbStatus)))
        {
            LOGFILE("Failed to read 0x%lX bytes long status block!", sizeof(UsbStatus));
            ret = false;
            goto end;
        }
        
        cmd_status = (UsbStatus*)g_usbTransferBuffer;
        
        if (cmd_status->magic != __builtin_bswap32(USB_CMD_HEADER_MAGIC))
        {
            LOGFILE("Invalid status block magic word!");
            ret = false;
            goto end;
        }
        
        ret = (cmd_status->status == UsbStatusType_Success);
        if (!ret) usbLogStatusDetail(cmd_status->status);
    }
    
end:
    if (!ret) g_usbTransferRemainingSize = 0;
    
    rwlockWriteUnlock(&(g_usbDeviceInterface.lock));
    rwlockWriteUnlock(&g_usbDeviceLock);
    
    return ret;
}

static bool usbCreateDetectionThread(void)
{
    if (thrd_create(&g_usbDetectionThread, usbDetectionThreadFunc, NULL) != thrd_success)
    {
        LOGFILE("Failed to create USB detection thread!");
        return false;
    }
    
    return true;
}

static void usbDestroyDetectionThread(void)
{
    /* Signal the exit event to terminate the USB detection thread */
    ueventSignal(&g_usbDetectionThreadExitEvent);
    
    /* Wait for the USB detection thread to exit. */
    thrd_join(g_usbDetectionThread, NULL);
}

static int usbDetectionThreadFunc(void *arg)
{
    (void)arg;
    
    Result rc = 0;
    int idx = 0;
    
    Waiter usb_change_event_waiter = waiterForEvent(g_usbStateChangeEvent);
    Waiter usb_timeout_event_waiter = waiterForUEvent(&g_usbTimeoutEvent);
    Waiter exit_event_waiter = waiterForUEvent(&g_usbDetectionThreadExitEvent);
    
    while(true)
    {
        /* Wait until an event is triggered. */
        rc = waitMulti(&idx, -1, usb_change_event_waiter, usb_timeout_event_waiter, exit_event_waiter);
        if (R_FAILED(rc)) continue;
        
        rwlockWriteLock(&g_usbDeviceLock);
        rwlockWriteLock(&(g_usbDeviceInterface.lock));
        
        /* Exit event triggered. */
        if (idx == 2) break;
        
        /* Retrieve current USB connection status. */
        /* Only proceed if we're dealing with a status change. */
        g_usbHostAvailable = usbIsHostAvailable();
        g_usbSessionStarted = false;
        g_usbTransferRemainingSize = 0;
        
        /* Start an USB session if we're connected to a host device. */
        /* This will essentially hang this thread and all other threads that call USB-related functions until: */
        /* a) A session is established. */
        /* b) The console is disconnected. */
        /* c) The thread exit event is triggered. */
        if (g_usbHostAvailable)
        {
            /* Wait until a session is established. */
            g_usbSessionStarted = usbStartSession();
            
            /* Check if the exit event was triggered while waiting for a session to be established. */
            if (!g_usbSessionStarted && g_usbDetectionThreadExitFlag) break;
        }
        
        rwlockWriteUnlock(&(g_usbDeviceInterface.lock));
        rwlockWriteUnlock(&g_usbDeviceLock);
    }
    
    /* Close USB session if needed. */
    if (g_usbHostAvailable && g_usbSessionStarted) usbEndSession();
    g_usbHostAvailable = g_usbSessionStarted = g_usbDetectionThreadExitFlag = false;
    g_usbTransferRemainingSize = 0;
    
    rwlockWriteUnlock(&(g_usbDeviceInterface.lock));
    rwlockWriteUnlock(&g_usbDeviceLock);
    
    return 0;
}

static bool usbStartSession(void)
{
    UsbCommandStartSession *cmd_block = NULL;
    size_t cmd_size = 0;
    u32 status = UsbStatusType_Success;
    
    if (!g_usbTransferBuffer || !g_usbDeviceInterfaceInitialized || !g_usbDeviceInterface.initialized)
    {
        LOGFILE("Invalid parameters!");
        return false;
    }
    
    usbPrepareCommandHeader(UsbCommandType_StartSession, (u32)sizeof(UsbCommandStartSession));
    
    cmd_block = (UsbCommandStartSession*)(g_usbTransferBuffer + sizeof(UsbCommandHeader));
    memset(cmd_block, 0, sizeof(UsbCommandStartSession));
    
    cmd_block->app_ver_major = VERSION_MAJOR;
    cmd_block->app_ver_minor = VERSION_MINOR;
    cmd_block->app_ver_micro = VERSION_MICRO;
    cmd_block->abi_version = USB_ABI_VERSION;
    
    cmd_size = (sizeof(UsbCommandHeader) + sizeof(UsbCommandStartSession));
    
    status = usbSendCommand(cmd_size);
    if (status != UsbStatusType_Success) usbLogStatusDetail(status);
    
    return (status == UsbStatusType_Success);
}

static void usbEndSession(void)
{
    if (!g_usbTransferBuffer || !g_usbDeviceInterfaceInitialized || !g_usbDeviceInterface.initialized || !g_usbHostAvailable || !g_usbSessionStarted)
    {
        LOGFILE("Invalid parameters!");
        return;
    }
    
    usbPrepareCommandHeader(UsbCommandType_EndSession, 0);
    
    if (!usbWrite(g_usbTransferBuffer, sizeof(UsbCommandHeader))) LOGFILE("Failed to send EndSession command!");
}

NX_INLINE void usbPrepareCommandHeader(u32 cmd, u32 cmd_block_size)
{
    if (cmd > UsbCommandType_EndSession) return;
    UsbCommandHeader *cmd_header = (UsbCommandHeader*)g_usbTransferBuffer;
    memset(cmd_header, 0, sizeof(UsbCommandHeader));
    cmd_header->magic = __builtin_bswap32(USB_CMD_HEADER_MAGIC);
    cmd_header->cmd = cmd;
    cmd_header->cmd_block_size = cmd_block_size;
}

static u32 usbSendCommand(size_t cmd_size)
{
    u32 cmd = ((UsbCommandHeader*)g_usbTransferBuffer)->cmd;
    UsbStatus *cmd_status = NULL;
    
    if (cmd_size < sizeof(UsbCommandHeader) || cmd_size > USB_TRANSFER_BUFFER_SIZE)
    {
        LOGFILE("Invalid command size!");
        return UsbStatusType_InvalidCommandSize;
    }
    
    if (!usbWrite(g_usbTransferBuffer, cmd_size))
    {
        /* Log error message only if the USB session has been started, or if thread exit flag hasn't been enabled. */
        if (g_usbSessionStarted || !g_usbDetectionThreadExitFlag) LOGFILE("Failed to write 0x%lX bytes long block for type 0x%X command!", cmd_size, cmd);
        return UsbStatusType_WriteCommandFailed;
    }
    
    if (!usbRead(g_usbTransferBuffer, sizeof(UsbStatus)))
    {
        /* Log error message only if the USB session has been started, or if thread exit flag hasn't been enabled. */
        if (g_usbSessionStarted || !g_usbDetectionThreadExitFlag) LOGFILE("Failed to read 0x%lX bytes long status block for type 0x%X command!", sizeof(UsbStatus), cmd);
        return UsbStatusType_ReadStatusFailed;
    }
    
    cmd_status = (UsbStatus*)g_usbTransferBuffer;
    
    if (cmd_status->magic != __builtin_bswap32(USB_CMD_HEADER_MAGIC))
    {
        LOGFILE("Invalid status block magic word for type 0x%X command!", cmd);
        return UsbStatusType_InvalidMagicWord;
    }
    
    return cmd_status->status;
}

NX_INLINE void usbLogStatusDetail(u32 status)
{
    switch(status)
    {
        case UsbStatusType_Success:
        case UsbStatusType_InvalidCommandSize:
        case UsbStatusType_WriteCommandFailed:
        case UsbStatusType_ReadStatusFailed:
            break;
        case UsbStatusType_InvalidMagicWord:
            LOGFILE("Host replied with Invalid Magic Word status code.");
            break;
        case UsbStatusType_UnsupportedCommand:
            LOGFILE("Host replied with Unsupported Command status code.");
            break;
        case UsbStatusType_UnsupportedAbiVersion:
            LOGFILE("Host replied with Unsupported ABI Version status code.");
            break;
        case UsbStatusType_MalformedCommand:
            LOGFILE("Host replied with Malformed Command status code.");
            break;
        case UsbStatusType_HostIoError:
            LOGFILE("Host replied with I/O Error status code.");
            break;
        default:
            LOGFILE("Unknown status code: 0x%X.", status);
            break;
    }
}

NX_INLINE bool usbAllocateTransferBuffer(void)
{
    if (g_usbTransferBuffer) return true;
    g_usbTransferBuffer = memalign(USB_TRANSFER_ALIGNMENT, USB_TRANSFER_BUFFER_SIZE);
    return (g_usbTransferBuffer != NULL);
}

NX_INLINE void usbFreeTransferBuffer(void)
{
    if (!g_usbTransferBuffer) return;
    free(g_usbTransferBuffer);
    g_usbTransferBuffer = NULL;
}

static bool usbInitializeComms(void)
{
    Result rc = 0;
    
    bool ret = (g_usbDeviceInterfaceInitialized && g_usbDeviceInterface.initialized);
    if (ret) goto end;
    
    rc = usbDsInitialize();
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInitialize failed! (0x%08X).", rc);
        goto end;
    }
    
    if (hosversionAtLeast(5,0,0))
    {
        u8 manufacturer = 0, product = 0, serial_number = 0;
        static const u16 supported_langs[1] = { 0x0409 };
        
        /* Send language descriptor. */
        rc = usbDsAddUsbLanguageStringDescriptor(NULL, supported_langs, sizeof(supported_langs) / sizeof(u16));
        if (R_FAILED(rc)) LOGFILE("usbDsAddUsbLanguageStringDescriptor failed! (0x%08X).", rc);
        
        /* Send manufacturer. */
        if (R_SUCCEEDED(rc))
        {
            rc = usbDsAddUsbStringDescriptor(&manufacturer, APP_AUTHOR);
            if (R_FAILED(rc)) LOGFILE("usbDsAddUsbStringDescriptor failed! (0x%08X) (manufacturer).", rc);
        }
        
        /* Send product. */
        if (R_SUCCEEDED(rc))
        {
            rc = usbDsAddUsbStringDescriptor(&product, APP_TITLE);
            if (R_FAILED(rc)) LOGFILE("usbDsAddUsbStringDescriptor failed! (0x%08X) (product).", rc);
        }
        
        /* Send serial number. */
        if (R_SUCCEEDED(rc))
        {
            rc = usbDsAddUsbStringDescriptor(&serial_number, APP_VERSION);
            if (R_FAILED(rc)) LOGFILE("usbDsAddUsbStringDescriptor failed! (0x%08X) (serial number).", rc);
        }
        
        /* Send device descriptors. */
        struct usb_device_descriptor device_descriptor = {
            .bLength = USB_DT_DEVICE_SIZE,
            .bDescriptorType = USB_DT_DEVICE,
            .bcdUSB = 0x0110,
            .bDeviceClass = 0x00,
            .bDeviceSubClass = 0x00,
            .bDeviceProtocol = 0x00,
            .bMaxPacketSize0 = 0x40,
            .idVendor = 0x057e,
            .idProduct = 0x3000,
            .bcdDevice = 0x0100,
            .iManufacturer = manufacturer,
            .iProduct = product,
            .iSerialNumber = serial_number,
            .bNumConfigurations = 0x01
        };
        
        /* Full Speed is USB 1.1. */
        if (R_SUCCEEDED(rc))
        {
            rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &device_descriptor);
            if (R_FAILED(rc)) LOGFILE("usbDsSetUsbDeviceDescriptor failed! (0x%08X) (USB 1.1).", rc);
        }
        
        /* High Speed is USB 2.0. */
        device_descriptor.bcdUSB = 0x0200;
        if (R_SUCCEEDED(rc))
        {
            rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device_descriptor);
            if (R_FAILED(rc)) LOGFILE("usbDsSetUsbDeviceDescriptor failed! (0x%08X) (USB 2.0).", rc);
        }
        
        /* Super Speed is USB 3.0. */
        /* Upgrade packet size to 512. */
        device_descriptor.bcdUSB = 0x0300;
        device_descriptor.bMaxPacketSize0 = 0x09;
        if (R_SUCCEEDED(rc))
        {
            rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device_descriptor);
            if (R_FAILED(rc)) LOGFILE("usbDsSetUsbDeviceDescriptor failed! (0x%08X) (USB 3.0).", rc);
        }
        
        /* Define Binary Object Store. */
        u8 bos[0x16] = {
            /* USB 1.1. */
            0x05,                       /* bLength. */
            USB_DT_BOS,                 /* bDescriptorType. */
            0x16, 0x00,                 /* wTotalLength. */
            0x02,                       /* bNumDeviceCaps. */
            
            /* USB 2.0. */
            0x07,                       /* bLength. */
            USB_DT_DEVICE_CAPABILITY,   /* bDescriptorType. */
            0x02,                       /* bDevCapabilityType. */
            0x02, 0x00, 0x00, 0x00,     /* dev_capability_data. */
            
            /* USB 3.0. */
            0x0A,                       /* bLength. */
            USB_DT_DEVICE_CAPABILITY,   /* bDescriptorType. */
            0x03,                       /* bDevCapabilityType. */
            0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00
        };
        
        if (R_SUCCEEDED(rc))
        {
            rc = usbDsSetBinaryObjectStore(bos, sizeof(bos));
            if (R_FAILED(rc)) LOGFILE("usbDsSetBinaryObjectStore failed! (0x%08X).", rc);
        }
    } else {
        static const UsbDsDeviceInfo device_info = {
            .idVendor = 0x057e,
            .idProduct = 0x3000,
            .bcdDevice = 0x0100,
            .Manufacturer = APP_AUTHOR,
            .Product = APP_TITLE,
            .SerialNumber = APP_VERSION
        };
        
        /* Set VID, PID and BCD. */
        rc = usbDsSetVidPidBcd(&device_info);
        if (R_FAILED(rc)) LOGFILE("usbDsSetVidPidBcd failed! (0x%08X).", rc);
    }
    
    if (R_FAILED(rc)) goto end;
    
    /* Initialize USB device interface. */
    rwlockWriteLock(&(g_usbDeviceInterface.lock));
    rwlockWriteLock(&(g_usbDeviceInterface.lock_in));
    rwlockWriteLock(&(g_usbDeviceInterface.lock_out));
    
    bool dev_iface_init = usbInitializeDeviceInterface();
    
    rwlockWriteUnlock(&(g_usbDeviceInterface.lock_out));
    rwlockWriteUnlock(&(g_usbDeviceInterface.lock_in));
    rwlockWriteUnlock(&(g_usbDeviceInterface.lock));
    
    if (!dev_iface_init)
    {
        LOGFILE("Failed to initialize USB device interface!");
        goto end;
    }
    
    if (hosversionAtLeast(5,0,0))
    {
        rc = usbDsEnable();
        if (R_FAILED(rc))
        {
            LOGFILE("usbDsEnable failed! (0x%08X).", rc);
            goto end;
        }
    }
    
    ret = g_usbDeviceInterfaceInitialized = true;
    
end:
    if (!ret) usbCloseComms();
    
    return ret;
}

static void usbCloseComms(void)
{
    usbDsExit();
    g_usbDeviceInterfaceInitialized = false;
    usbFreeDeviceInterface();
}

static void usbFreeDeviceInterface(void)
{
    rwlockWriteLock(&(g_usbDeviceInterface.lock));
    
    if (!g_usbDeviceInterface.initialized) {
        rwlockWriteUnlock(&(g_usbDeviceInterface.lock));
        return;
    }
    
    rwlockWriteLock(&(g_usbDeviceInterface.lock_in));
    rwlockWriteLock(&(g_usbDeviceInterface.lock_out));
    
    g_usbDeviceInterface.initialized = false;
    
    g_usbDeviceInterface.interface = NULL;
    g_usbDeviceInterface.endpoint_in = NULL;
    g_usbDeviceInterface.endpoint_out = NULL;
    
    rwlockWriteUnlock(&(g_usbDeviceInterface.lock_out));
    rwlockWriteUnlock(&(g_usbDeviceInterface.lock_in));
    
    rwlockWriteUnlock(&(g_usbDeviceInterface.lock));
}

NX_INLINE bool usbInitializeDeviceInterface(void)
{
    return (hosversionAtLeast(5,0,0) ? usbInitializeDeviceInterface5x() : usbInitializeDeviceInterface1x());
}

static bool usbInitializeDeviceInterface5x(void)
{
    Result rc = 0;
    
    struct usb_interface_descriptor interface_descriptor = {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = 4,
        .bNumEndpoints = 2,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
    };
    
    struct usb_endpoint_descriptor endpoint_descriptor_in = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x40,
    };
    
    struct usb_endpoint_descriptor endpoint_descriptor_out = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x40,
    };
    
    struct usb_ss_endpoint_companion_descriptor endpoint_companion = {
        .bLength = sizeof(struct usb_ss_endpoint_companion_descriptor),
        .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst = 0x0F,
        .bmAttributes = 0x00,
        .wBytesPerInterval = 0x00,
    };
    
    /* Enable device interface. */
    g_usbDeviceInterface.initialized = true;
    
    /* Setup interface. */
    rc = usbDsRegisterInterface(&(g_usbDeviceInterface.interface));
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsRegisterInterface failed! (0x%08X).", rc);
        return false;
    }
    
    interface_descriptor.bInterfaceNumber = g_usbDeviceInterface.interface->interface_index;
    endpoint_descriptor_in.bEndpointAddress += (interface_descriptor.bInterfaceNumber + 1);
    endpoint_descriptor_out.bEndpointAddress += (interface_descriptor.bInterfaceNumber + 1);
    
    /* Full Speed config (USB 1.1). */
    rc = usbDsInterface_AppendConfigurationData(g_usbDeviceInterface.interface, UsbDeviceSpeed_Full, &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_AppendConfigurationData failed! (0x%08X) (USB 1.1) (interface).", rc);
        return false;
    }
    
    rc = usbDsInterface_AppendConfigurationData(g_usbDeviceInterface.interface, UsbDeviceSpeed_Full, &endpoint_descriptor_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_AppendConfigurationData failed! (0x%08X) (USB 1.1) (in endpoint).", rc);
        return false;
    }
    
    rc = usbDsInterface_AppendConfigurationData(g_usbDeviceInterface.interface, UsbDeviceSpeed_Full, &endpoint_descriptor_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_AppendConfigurationData failed! (0x%08X) (USB 1.1) (out endpoint).", rc);
        return false;
    }
    
    /* High Speed config (USB 2.0). */
    endpoint_descriptor_in.wMaxPacketSize = 0x200;
    endpoint_descriptor_out.wMaxPacketSize = 0x200;
    
    rc = usbDsInterface_AppendConfigurationData(g_usbDeviceInterface.interface, UsbDeviceSpeed_High, &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_AppendConfigurationData failed! (0x%08X) (USB 2.0) (interface).", rc);
        return false;
    }
    
    rc = usbDsInterface_AppendConfigurationData(g_usbDeviceInterface.interface, UsbDeviceSpeed_High, &endpoint_descriptor_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_AppendConfigurationData failed! (0x%08X) (USB 2.0) (in endpoint).", rc);
        return false;
    }
    
    rc = usbDsInterface_AppendConfigurationData(g_usbDeviceInterface.interface, UsbDeviceSpeed_High, &endpoint_descriptor_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_AppendConfigurationData failed! (0x%08X) (USB 2.0) (out endpoint).", rc);
        return false;
    }
    
    /* Super Speed config (USB 3.0). */
    endpoint_descriptor_in.wMaxPacketSize = 0x400;
    endpoint_descriptor_out.wMaxPacketSize = 0x400;
    
    rc = usbDsInterface_AppendConfigurationData(g_usbDeviceInterface.interface, UsbDeviceSpeed_Super, &interface_descriptor, USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_AppendConfigurationData failed! (0x%08X) (USB 3.0) (interface).", rc);
        return false;
    }
    
    rc = usbDsInterface_AppendConfigurationData(g_usbDeviceInterface.interface, UsbDeviceSpeed_Super, &endpoint_descriptor_in, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_AppendConfigurationData failed! (0x%08X) (USB 3.0) (in endpoint).", rc);
        return false;
    }
    
    rc = usbDsInterface_AppendConfigurationData(g_usbDeviceInterface.interface, UsbDeviceSpeed_Super, &endpoint_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_AppendConfigurationData failed! (0x%08X) (USB 3.0) (in endpoint companion).", rc);
        return false;
    }
    
    rc = usbDsInterface_AppendConfigurationData(g_usbDeviceInterface.interface, UsbDeviceSpeed_Super, &endpoint_descriptor_out, USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_AppendConfigurationData failed! (0x%08X) (USB 3.0) (out endpoint).", rc);
        return false;
    }
    
    rc = usbDsInterface_AppendConfigurationData(g_usbDeviceInterface.interface, UsbDeviceSpeed_Super, &endpoint_companion, USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_AppendConfigurationData failed! (0x%08X) (USB 3.0) (out endpoint companion).", rc);
        return false;
    }
    
    /* Setup endpoints. */
    rc = usbDsInterface_RegisterEndpoint(g_usbDeviceInterface.interface, &(g_usbDeviceInterface.endpoint_in), endpoint_descriptor_in.bEndpointAddress);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_RegisterEndpoint failed! (0x%08X) (in endpoint).", rc);
        return false;
    }
    
    rc = usbDsInterface_RegisterEndpoint(g_usbDeviceInterface.interface, &(g_usbDeviceInterface.endpoint_out), endpoint_descriptor_out.bEndpointAddress);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_RegisterEndpoint failed! (0x%08X) (out endpoint).", rc);
        return false;
    }
    
    rc = usbDsInterface_EnableInterface(g_usbDeviceInterface.interface);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_EnableInterface failed! (0x%08X).", rc);
        return false;
    }
    
    return true;
}

static bool usbInitializeDeviceInterface1x(void)
{
    Result rc = 0;
    
    struct usb_interface_descriptor interface_descriptor = {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = 0,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
    };
    
    struct usb_endpoint_descriptor endpoint_descriptor_in = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x200,
    };
    
    struct usb_endpoint_descriptor endpoint_descriptor_out = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x200,
    };
    
    /* Enable device interface. */
    g_usbDeviceInterface.initialized = true;
    
    /* Setup interface. */
    rc = usbDsGetDsInterface(&(g_usbDeviceInterface.interface), &interface_descriptor, "usb");
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsGetDsInterface failed! (0x%08X).", rc);
        return false;
    }
    
    /* Setup endpoints. */
    rc = usbDsInterface_GetDsEndpoint(g_usbDeviceInterface.interface, &(g_usbDeviceInterface.endpoint_in), &endpoint_descriptor_in);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_GetDsEndpoint failed! (0x%08X) (in endpoint).", rc);
        return false;
    }
    
    rc = usbDsInterface_GetDsEndpoint(g_usbDeviceInterface.interface, &(g_usbDeviceInterface.endpoint_out), &endpoint_descriptor_out);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_GetDsEndpoint failed! (0x%08X) (out endpoint).", rc);
        return false;
    }
    
    rc = usbDsInterface_EnableInterface(g_usbDeviceInterface.interface);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsInterface_EnableInterface failed! (0x%08X).", rc);
        return false;
    }
    
    return true;
}

NX_INLINE bool usbIsHostAvailable(void)
{
    u32 state = 0;
    Result rc = usbDsGetState(&state);
    return (R_SUCCEEDED(rc) && state == 5);
}

NX_INLINE bool usbRead(void *buf, u64 size)
{
    rwlockWriteLock(&(g_usbDeviceInterface.lock_out));
    bool ret = usbTransferData(buf, size, g_usbDeviceInterface.endpoint_out);
    rwlockWriteUnlock(&(g_usbDeviceInterface.lock_out));
    return ret;
}

NX_INLINE bool usbWrite(void *buf, u64 size)
{
    rwlockWriteLock(&(g_usbDeviceInterface.lock_in));
    bool ret = usbTransferData(buf, size, g_usbDeviceInterface.endpoint_in);
    if (ret) usbDsEndpoint_SetZlt(g_usbDeviceInterface.endpoint_in, true);
    rwlockWriteUnlock(&(g_usbDeviceInterface.lock_in));
    return ret;
}

static bool usbTransferData(void *buf, u64 size, UsbDsEndpoint *endpoint)
{
    if (!buf || !IS_ALIGNED((u64)buf, USB_TRANSFER_ALIGNMENT) || !size || !endpoint)
    {
        LOGFILE("Invalid parameters!");
        return false;
    }
    
    if (!usbIsHostAvailable())
    {
        LOGFILE("USB host unavailable!");
        return false;
    }
    
    u32 urb_id = 0;
    Result rc = 0;
    UsbDsReportData report_data = {0};
    u32 transferred_size = 0;
    bool thread_exit = false;
    
    /* Start an USB transfer using the provided endpoint. */
    rc = usbDsEndpoint_PostBufferAsync(endpoint, buf, size, &urb_id);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsEndpoint_PostBufferAsync failed! (0x%08X).", rc);
        return false;
    }
    
    /* Wait for the transfer to finish. */
    if (g_usbSessionStarted)
    {
        /* If the USB transfer session has already been started, then use a regular timeout value. */
        rc = eventWait(&(endpoint->CompletionEvent), USB_TRANSFER_TIMEOUT * (u64)1000000000);
    } else {
        /* If we're starting an USB transfer session, wait indefinitely inside a loop to let the user start the companion app. */
        int idx = 0;
        Waiter completion_event_waiter = waiterForEvent(&(endpoint->CompletionEvent));
        Waiter exit_event_waiter = waiterForUEvent(&g_usbDetectionThreadExitEvent);
        
        rc = waitMulti(&idx, -1, completion_event_waiter, exit_event_waiter);
        if (R_SUCCEEDED(rc) && idx == 1)
        {
            /* Exit event triggered. */
            rc = MAKERESULT(Module_Kernel, KernelError_TimedOut);
            g_usbDetectionThreadExitFlag = thread_exit = true;
        }
    }
    
    /* Clear the endpoint completion event. */
    if (!thread_exit) eventClear(&(endpoint->CompletionEvent));
    
    if (R_FAILED(rc))
    {
        /* Cancel transfer. */
        usbDsEndpoint_Cancel(endpoint);
        
        /* Safety measure: wait until the completion event is triggered again before proceeding. */
        eventWait(&(endpoint->CompletionEvent), UINT64_MAX);
        eventClear(&(endpoint->CompletionEvent));
        
        /* Signal usermode USB timeout event if needed. */
        /* This will "reset" the USB connection by making the background thread wait until a new session is established. */
        if (g_usbSessionStarted) ueventSignal(&g_usbTimeoutEvent);
        
        if (!thread_exit) LOGFILE("eventWait failed! (0x%08X).", rc);
        return false;
    }
    
    rc = usbDsEndpoint_GetReportData(endpoint, &report_data);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsEndpoint_GetReportData failed! (0x%08X).", rc);
        return false;
    }
    
    rc = usbDsParseReportData(&report_data, urb_id, NULL, &transferred_size);
    if (R_FAILED(rc))
    {
        LOGFILE("usbDsParseReportData failed! (0x%08X).", rc);
        return false;
    }
    
    if (transferred_size != size)
    {
        LOGFILE("USB transfer failed! Expected 0x%lX bytes, got 0x%X bytes.", size, transferred_size);
        return false;
    }
    
    return true;
}
