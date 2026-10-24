/* SPDX-License-Identifier: MIT */
/*
 * IOKit glue for the A16 built-in keyboard and touchpad: one SnapI2CHIDBus per
 * guest device-tree node (compatible "snapintosh,geni-i2c-hid") drives the
 * node's GENI I2C serial engine through the stage-2 passthrough page in reg[0]
 * and publishes one SnapI2CHIDDevice (an IOHIDDevice) for the I2C-HID device
 * the node names. IOHIDFamily then builds IOHIDInterface and IOHIDEventDriver
 * on it as for any HID device. All bus traffic runs on the bus's own work
 * loop; the transport and state machine are the hardware-independent files
 * next to this one (geni-i2c.c, i2c-hid.c, hid-rdesc.c), which
 * tools/test-snap-i2c-hid.c exercises on the host.
 *
 * Node properties (all little-endian 32-bit cells unless noted):
 *   reg[0]                              GENI serial-engine page (>= 4 KiB)
 *   reg[1]                              optional read-only TLMM GPIO page
 *   snapintosh,hid-address              7-bit I2C address
 *   snapintosh,hid-descriptor-reg  HID descriptor register (ACPI/DT)
 *   snapintosh,hid-vendor-product       optional expected VID, PID
 *   snapintosh,bus-hz                   optional, default 400000
 *   snapintosh,poll-us                  optional, default 8000
 *   snapintosh,irq-policy               optional, 0 events / 1 masked (default 1)
 *   snapintosh,attention                optional: offset in reg[1], active-low flag
 *   snapintosh,hid-product              optional string
 */
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
/* The in-kernel IOHIDDevice is marked "Use DriverKit"; it is still what the
 * J700 kernel collection's IOHIDFamily 2.0.0 exports, and a dext would need
 * the DriverKit runtime this guest does not run yet. */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDDeviceKeys.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hidevent/IOHIDEventService.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSData.h>

extern "C" {
#include "geni-i2c.h"
#include "i2c-hid.h"
#include "hid-keyboard.h"
#include "hid-pointer.h"
#include "hid-ptp.h"
#include "hid-mt2.h"
}

#define LOG_PREFIX "SnapI2CHID: "

/* agents/input-latency FAILURE-MODES.md T1-T9: -DSNAP_I2C_HID_LATENCY=1 schedules each RUNNING poll one period after
 * the previous one began, not after it ended. b12v6 measured 12.0-12.2 ms between 8 ms polls: the step's own transfer
 * and the timer's slop added about 4 ms. It also polls every SNAP_I2C_HID_ACTIVE_POLL_US while the device sent a report
 * in the last SNAP_I2C_HID_ACTIVE_HOLD_US (boot-arg snap_i2c_active_us; 0 = the configured period throughout), and
 * waits a transfer's wire time to the microsecond instead of rounding it up to whole milliseconds of IOSleep. Off by
 * default; without it this file compiles as before. */
#ifndef SNAP_I2C_HID_LATENCY
#define SNAP_I2C_HID_LATENCY 0
#endif
#if SNAP_I2C_HID_LATENCY
#include <pexpert/pexpert.h>
#ifndef SNAP_I2C_HID_ACTIVE_POLL_US
#define SNAP_I2C_HID_ACTIVE_POLL_US 4000u
#endif
#define SNAP_I2C_HID_ACTIVE_HOLD_US 500000u     /* T1: active for this long after a report */
#define SNAP_I2C_HID_MIN_POLL_US 2000u          /* T1: the floor of the active period */
#endif

/* -DSNAP_I2C_HID_FNKEYS=1, in this unit and hid-keyboard.o both: the keyboard's Fn-key layer (hid-keyboard.h), which
 * turns report 90's ASUS hotkeys into macOS's usages and can log what the F-row sends. Even built in it stays off
 * unless the boot-arg snap_i2c_fnkeys sets HID_FNKEYS_* bits. Off by default; without it this file compiles as
 * before. */
#ifndef SNAP_I2C_HID_FNKEYS
#define SNAP_I2C_HID_FNKEYS 0
#endif
#if SNAP_I2C_HID_FNKEYS && !SNAP_I2C_HID_LATENCY
#include <pexpert/pexpert.h>
#endif

/* The engine's scroll options are IOHIDEventService's dispatch options (26A428 header values). */
static_assert(HID_PTP_SCROLL_PHASE_BEGAN == kHIDDispatchOptionPhaseBegan &&
              HID_PTP_SCROLL_PHASE_CHANGED == kHIDDispatchOptionPhaseChanged &&
              HID_PTP_SCROLL_PHASE_ENDED == kHIDDispatchOptionPhaseEnded &&
              HID_PTP_SCROLL_MOMENTUM_START == kHIDDispatchOptionScrollMomentumStart &&
              HID_PTP_SCROLL_MOMENTUM_CONTINUE == kHIDDispatchOptionScrollMomentumContinue &&
              HID_PTP_SCROLL_MOMENTUM_END == kHIDDispatchOptionScrollMomentumEnd,
              "hid-ptp.h scroll options differ from kHIDDispatchOption*");

/* Feature reports for the Precision Touchpad switch. tools/test-snap-i2c-hid-start.cpp defines
 * these two to its model of the touchpad's features. */
#ifndef SNAP_I2C_HID_FEATURE_GET
#define SNAP_I2C_HID_FEATURE_GET(dev, id, buf, cap, got) \
    i2c_hid_get_report((dev), I2C_HID_TYPE_FEATURE, (id), (buf), (cap), (got))
#define SNAP_I2C_HID_FEATURE_SET(dev, report, len) i2c_hid_set_report((dev), I2C_HID_TYPE_FEATURE, (report), (len))
#endif

/* The latest non-modifier key-down, in microseconds of uptime, from any SnapI2CHIDKeyboard: the
 * touchpad takes no tap within HID_PTP_TYPING_US of it (failure mode F17). 0 = none yet. */
static uint64_t gSnapLastKeyDownUs;

static uint64_t uptimeUs(uint64_t abs)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(abs, &ns);
    return ns / 1000u;
}

class SnapI2CHIDDevice;
class SnapI2CHIDKeyboard;
class SnapI2CHIDPointer;
class SnapI2CHIDMultitouch;

class SnapI2CHIDBus : public IOService {
    OSDeclareDefaultStructors(SnapI2CHIDBus)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    /* Called by SnapI2CHIDDevice, any thread. */
    IOReturn request(uint32_t kind, uint8_t type, uint8_t id, uint8_t *buf, uint32_t len,
                     uint32_t *got);
    const struct i2c_hid_dev *device() const { return fDev; }
    uint32_t location() const { return fLocation; }
    /* Any thread: an event driver (a kernel service, not a user client) opened or closed our
     * IOHIDDevice. The bus applies it on its next step (yieldToEventDriver). */
    void noteEventDriver(const void *client, const char *cls, bool open);
    OSString *copyProduct() const;

    enum { kRequestOutput = 1, kRequestSetReport = 2, kRequestGetReport = 3 };

private:
    static uint32_t ioRead32(void *ctx, uint32_t offset);
    static void ioWrite32(void *ctx, uint32_t offset, uint32_t value);
    static uint64_t ioNowUs(void *ctx);
    static void ioDelayUs(void *ctx, uint32_t us);
    static void deliver(void *ctx, const uint8_t *report, uint32_t len);
    friend class SnapI2CHIDKeyboard;
    friend class SnapI2CHIDPointer;
    friend class SnapI2CHIDMultitouch;
    static int attention(void *ctx);
    static void timerAction(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn gatedStart(OSObject *owner, void *, void *, void *, void *);
    static IOReturn gatedStop(OSObject *owner, void *, void *, void *, void *);
    static IOReturn gatedRun(OSObject *owner, void *hid, void *mt, void *, void *);
    static IOReturn gatedRequest(OSObject *owner, void *req, void *, void *, void *);
    static IOReturn gatedPtp(OSObject *owner, void *setup, void *, void *, void *);
    static int ptpGet(void *ctx, uint8_t id, uint8_t *buf, uint32_t cap, uint32_t *got);
    static int ptpSet(void *ctx, const uint8_t *report, uint32_t len);
    static void publishCall(thread_call_param_t owner, thread_call_param_t);
    void publish();
    bool enablePrecisionTouchpad();
    void notePtpReinit();

    void step();
#if SNAP_I2C_HID_LATENCY
    void noteWake(uint64_t startUs);
    void armNextStep(uint32_t wake, uint64_t startUs, uint32_t reports);
#endif
#if SNAP_I2C_HID_FNKEYS
    void noteRejectedInput();
#endif
    void noteState();
    void noteProgress();
    void noteServices();
    void yieldToEventDriver();
    uint32_t driverOpens() const;
    void noteMultitouch();
    bool multitouchWanted() const;
    SnapI2CHIDMultitouch *publishMultitouch();
    bool readU32(const char *key, uint32_t *out, uint32_t count = 1) const;
    void releaseAll();

    IOService *fProvider = nullptr;
    IOMemoryMap *fRegs = nullptr;
    IOMemoryMap *fGpio = nullptr;
    volatile uint32_t *fBase = nullptr;
    volatile uint32_t *fAttention = nullptr;
    uint32_t fAttentionActiveLow = 1;
    uint32_t fLocation = 0;
    uint32_t fBusHz = 400000;
    uint32_t fPolicy = GENI_I2C_IRQ_MASKED;
    IOWorkLoop *fWorkLoop = nullptr;
    IOTimerEventSource *fTimer = nullptr;
    IOCommandGate *fGate = nullptr;
    thread_call_t fPublish = nullptr;
    /* 1 from thread_call_enter until publishCall returns (or a cancel dequeued
     * it). thread_call_cancel_wait is private KPI, so stop() waits on this. */
    uint32_t fPublishing = 0;
    struct geni_i2c fBus;
    struct i2c_hid_config fCfg;
    struct i2c_hid_dev *fDev = nullptr;
    SnapI2CHIDDevice *fHID = nullptr;
    /* Our own keyboard event service (26A428's generic drivers are IOMatchDefer and none
     * attached in v40/v41): set on the work loop by gatedRun, used by deliver(). */
    SnapI2CHIDKeyboard *fKeyboard = nullptr;
    SnapI2CHIDKeyboard *fKeyboardStarted = nullptr;
    /* The touchpad's pointer, the same way: mouse mode (hid-pointer.c) and, once the device took
     * the switch at publish, Precision Touchpad mode (hid-ptp.c). */
    SnapI2CHIDPointer *fPointer = nullptr;
    SnapI2CHIDPointer *fPointerStarted = nullptr;
    struct hid_ptp_layout fPtp;
    bool fPtpArmed = false;
    bool fPtpReapply = false;            /* set by the pointer on report 1 while armed (F24) */
    uint32_t fPtpReinits = 0;            /* stats.reinits when the switch was last applied */
    uint8_t fPtpScratch[264];            /* the largest feature report it reads: the 257-byte blob */
    uint32_t fLastState = ~0u;
    uint64_t fStartUs = 0;
    uint32_t fNextStatsPoll = 125;      /* about 1 s of 8 ms polls, then 10 s, 60 s, every 10 min */
    uint32_t fLoggedReports = 0;
    uint64_t fRunningAtUs = 0;
    uint32_t fRegistryLogs = 0;         /* the HID stack above the device: at about 2 s and 30 s */
    /* Event drivers holding our IOHIDDevice open (handleOpen/handleClose, any thread; a client
     * opening twice is one entry), and whether step() last saw any: while one holds it, our own
     * services yield to it (Y1-Y5). */
    enum { kDriverSlots = 4 };
    uintptr_t fDriverClients[kDriverSlots] = {0, 0, 0, 0};
    uint32_t fDriverOpensApplied = 0;
    char fDriverName[48] = {0};
    bool fRawPublished = false;         /* the device's own IOHIDDevice (never for an armed touchpad) */
    /* The armed touchpad's Apple multitouch device (hid-mt2.c), set on the work loop by gatedRun,
     * and what the registry above it showed last (SnapI2CHIDMultitouch::kStack*). */
    SnapI2CHIDMultitouch *fMT = nullptr;
    uint32_t fMTStack = ~0u, fMTNextCheck = 0, fMTKicks = 0, fMTLastKick = 0;
    bool fStopping = false;
#if SNAP_I2C_HID_LATENCY
    /* T1-T3: the active period (0 = the configured one throughout), the current poll's deadline, the time the timer was
     * asked for and a decaying estimate of how late it fires, the last report, and the polls of each kind. */
    uint32_t fActiveUs = SNAP_I2C_HID_ACTIVE_POLL_US;
    uint64_t fDeadlineUs = 0, fArmedForUs = 0, fSlopUs = 0, fLastReportUs = 0;
    uint32_t fActivePolls = 0, fIdlePolls = 0;
#endif
#if SNAP_I2C_HID_FNKEYS
    uint32_t fRejectedSeen = 0;         /* bad_length + unknown_id + wrong_size at the last step */
#endif
};

class SnapI2CHIDDevice : public IOHIDDevice {
    OSDeclareDefaultStructors(SnapI2CHIDDevice)

public:
    bool initWithBus(SnapI2CHIDBus *bus);
    void free() override;
    bool handleStart(IOService *provider) override;
    IOReturn newReportDescriptor(IOMemoryDescriptor **descriptor) const override;
    OSString *newTransportString() const override;
    OSString *newProductString() const override;
    OSNumber *newVendorIDNumber() const override;
    OSNumber *newProductIDNumber() const override;
    OSNumber *newVersionNumber() const override;
    OSNumber *newLocationIDNumber() const override;
    OSNumber *newReportIntervalNumber() const override;
    using IOHIDDevice::getReport;
    using IOHIDDevice::setReport;
    IOReturn getReport(IOMemoryDescriptor *report, IOHIDReportType type,
                       IOOptionBits options) override;
    IOReturn setReport(IOMemoryDescriptor *report, IOHIDReportType type,
                       IOOptionBits options = 0) override;
    bool handleOpen(IOService *client, IOOptionBits options, void *argument) override;
    void handleClose(IOService *client, IOOptionBits options) override;
    void deliverReport(const uint8_t *report, uint32_t len);
    void logRegistry(const char *when);

private:
    void logRequest(const char *what, IOHIDReportType type, IOOptionBits options, IOByteCount len,
                    IOReturn result);

    SnapI2CHIDBus *fBus = nullptr;
    uint32_t fReportCalls = 0, fReportFailures = 0, fRequests = 0;
    OSData *fDescriptor = nullptr;
    OSString *fProduct = nullptr;
    IOBufferMemoryDescriptor *fReport = nullptr;
    uint16_t fVendor = 0, fProductID = 0, fVersion = 0;
    uint32_t fInterval = 0;
};

/* The armed touchpad as an Apple multitouch device (hid-mt2.h has the formats and their 26A428
 * sources): VendorID 0x05ac, ProductID 641, so Apple's own AppleMultitouchTrackpadHIDEventDriver
 * (J700 BootKC personality "Mac SPI MT - TopCase - 641", not deferred) matches its interface and
 * builds an AppleMultitouchDevice; its user-space clients (HSTouchHIDService, MultitouchSupport) then
 * make the pointer, scroll, clicks and every system gesture from the contact frames this device
 * sends. Input report 2 carries one frame per Precision Touchpad frame (from the pointer service's
 * engine hook) plus trailing frames on the bus tick; feature reports answer the driver's handshake
 * from memory and never touch the I2C device (M14). Our pointer service stays the fallback: it
 * hands the touchpad over only while a frames consumer is attached (M1, M6, M7). */
class SnapI2CHIDMultitouch : public IOHIDDevice {
    OSDeclareDefaultStructors(SnapI2CHIDMultitouch)

public:
    enum { kStackInterface = 1, kStackAppleDriver = 2, kStackOtherDriver = 4, kStackDevice = 8, kStackConsumer = 16 };

    bool initWithBus(SnapI2CHIDBus *bus, const struct hid_mt2 *geometry);
    void free() override;
    bool handleStart(IOService *provider) override;
    IOReturn newReportDescriptor(IOMemoryDescriptor **descriptor) const override;
    OSString *newTransportString() const override;
    OSString *newManufacturerString() const override;
    OSString *newProductString() const override;
    OSNumber *newVendorIDNumber() const override;
    OSNumber *newProductIDNumber() const override;
    OSNumber *newVersionNumber() const override;
    OSNumber *newLocationIDNumber() const override;
    OSNumber *newReportIntervalNumber() const override;
    using IOHIDDevice::getReport;
    using IOHIDDevice::setReport;
    IOReturn getReport(IOMemoryDescriptor *report, IOHIDReportType type, IOOptionBits options) override;
    IOReturn setReport(IOMemoryDescriptor *report, IOHIDReportType type, IOOptionBits options = 0) override;
    /* Bus work loop. */
    void frame(const struct hid_ptp_contact *c, uint32_t n, uint32_t buttons, uint32_t scan, int has_scan);
    void tick();
    void reset();
    void kick();
    uint32_t stack() const;             /* kStack* bits: the registry above this device */

private:
    void send(uint32_t len);
    void noteFrames();
    void logRequest(const char *what, IOHIDReportType type, uint8_t id, uint32_t len, IOReturn result);

    SnapI2CHIDBus *fBus = nullptr;
    struct hid_mt2 fMT;
    IOBufferMemoryDescriptor *fReport = nullptr;
    uint8_t fBuf[HID_MT2_MAX_REPORT];
    uint8_t fQuery = 0;                 /* report 1's pending query (set, then get) */
    uint32_t fSent = 0, fFailures = 0, fRequests = 0, fNextLog = 1;
};

/* The keyboard as an IOHIDEventService the HID event system consumes directly: report 54
 * becomes keyboard-page transitions and report 55 consumer-page ones (src/hid-keyboard.c),
 * dispatched with dispatchKeyboardEvent; the Caps Lock LED comes back as output report 54.
 * It is attached to the bus and started by it, not matched, so no deferred personality or
 * DriverKit server is involved. Key usages are never logged; only counts. The one exception is
 * the opt-in Fn-key raw log (SNAP_I2C_HID_FNKEYS, snap_i2c_fnkeys bit 0x2), which names hotkey
 * codes, Consumer usages and F-row/navigation keys, never a key that types a character. */
class SnapI2CHIDKeyboard : public IOHIDEventService {
    OSDeclareDefaultStructors(SnapI2CHIDKeyboard)

public:
    bool initWithBus(SnapI2CHIDBus *bus);
    void free() override;
    IOWorkLoop *getWorkLoop() const override;
    void handleReport(const uint8_t *report, uint32_t len);   /* bus work loop */
    void releaseKeys();                                        /* bus work loop */
    void suppress(const char *why);                            /* bus work loop */
    void resume();                                             /* bus work loop */
    const char *state() const { return fSuppressed ? "yielded" : "dispatching"; }
#if SNAP_I2C_HID_FNKEYS
    void noteRejected(const uint8_t *input, uint32_t len);     /* bus work loop */
    bool rawLog() const { return (fFn.flags & HID_FNKEYS_RAW_LOG) != 0; }
#endif

protected:
    bool handleStart(IOService *provider) override;
    OSString *getTransport() override;
    UInt32 getLocationID() override;
    UInt32 getVendorID() override;
    UInt32 getProductID() override;
    UInt32 getVersion() override;
    UInt32 getCountryCode() override;
    OSString *getManufacturer() override;
    OSString *getProduct() override;
    UInt32 getReportInterval() override;
    UInt32 getPrimaryUsagePage() override;
    UInt32 getPrimaryUsage() override;
    IOReturn setElementValue(UInt32 usagePage, UInt32 usage, UInt32 value) override;

private:
    static void emit(void *ctx, uint32_t page, uint32_t usage, uint32_t down);
#if SNAP_I2C_HID_FNKEYS
    void startFnKeys();
    bool mayLog();
    static void note(void *ctx, const struct hid_fnkeys_note *n);
#endif

    SnapI2CHIDBus *fBus = nullptr;
    OSString *fProductName = nullptr;
    struct hid_kbd_state fState;
    uint64_t fNow = 0;
    uint32_t fReports = 0, fTransitions = 0, fNextLog = 1, fLeds = 0, fLedRequests = 0;
    uint32_t fYieldedReports = 0, fNextYieldLog = 1;
    bool fSuppressed = false;
#if SNAP_I2C_HID_FNKEYS
    struct hid_fnkeys fFn;              /* flags 0: the layer is off and fState works as without it */
    struct hid_fnkeys_rate fRate;
#endif
};

/* The touchpad as an IOHIDEventService. In its default mouse mode the mouse collection's input
 * report (layout from the device's report descriptor, hid-pointer.c) becomes
 * dispatchRelativePointerEvent and dispatchScrollWheelEvent. When the bus switched the device to
 * Precision Touchpad mode, the multi-contact report goes through hid-ptp.c's engine instead:
 * dispatchRelativePointerEventWithFixed for one finger, clicks and taps, and phased
 * dispatchScrollWheelEventWithFixed (with momentum on the bus tick) for two; the service then
 * also carries the trackpad scroll identity (HIDScrollAccelerationType and the rest, section 4.4
 * of docs/A16-INPUT-PTP-20261020.md). Report 1 keeps today's path in both modes. Started by the
 * bus like the keyboard service; counts only in the log. */
class SnapI2CHIDPointer : public IOHIDEventService {
    OSDeclareDefaultStructors(SnapI2CHIDPointer)

public:
    bool initWithBus(SnapI2CHIDBus *bus, const struct hid_pointer_layout *layout,
                     const struct hid_ptp_layout *ptp = nullptr);
    void free() override;
    IOWorkLoop *getWorkLoop() const override;
    void handleReport(const uint8_t *report, uint32_t len);   /* bus work loop */
    void tick();                                               /* bus work loop, every poll */
    void resetTouch();                                         /* bus work loop */
    void releaseButtons();                                     /* bus work loop */
    void suppress(const char *why);                            /* bus work loop */
    void resume();                                             /* bus work loop */
    void handover(bool on);                                    /* bus work loop */
    const char *state() const
    {
        return fSuppressed ? "yielded" : fHandover ? "handed over to the multitouch stack" : "dispatching";
    }

protected:
    bool handleStart(IOService *provider) override;
    OSString *getTransport() override;
    UInt32 getLocationID() override;
    UInt32 getVendorID() override;
    UInt32 getProductID() override;
    UInt32 getVersion() override;
    UInt32 getCountryCode() override;
    OSString *getManufacturer() override;
    OSString *getProduct() override;
    UInt32 getReportInterval() override;
    UInt32 getPrimaryUsagePage() override;
    UInt32 getPrimaryUsage() override;

private:
    static void move(void *ctx, int32_t dx, int32_t dy, uint32_t buttons);
    static void scroll(void *ctx, int32_t vertical, int32_t horizontal);
    static void ptpPointer(void *ctx, int32_t dx, int32_t dy, uint32_t buttons);
    static void ptpScroll(void *ctx, int32_t vertical, int32_t horizontal, uint32_t options);
    static void ptpFrame(void *ctx, const struct hid_ptp_contact *c, uint32_t n, uint32_t buttons,
                         uint32_t scan, int has_scan);
    void noteEvent();
    void notePtp(int result);

    SnapI2CHIDBus *fBus = nullptr;
    OSString *fProductName = nullptr;
    struct hid_pointer_layout fLayout;
    struct hid_pointer_state fState;
    bool fHaveMouse = false, fPtpArmed = false;
    struct hid_ptp_engine fEngine;
    struct hid_ptp_sink fSink;
    uint64_t fNow = 0;
    uint32_t fReports = 0, fMoves = 0, fScrolls = 0, fNextLog = 1;
    uint32_t fMouseWhileArmed = 0, fNextPtpLog = 1;
    uint32_t fLoggedFrames = 0;                  /* the first frames' contacts, logged (D2) */
    uint32_t fYieldedReports = 0, fYieldedPtp = 0, fNextYieldLog = 1;
    uint32_t fHandedOver = 0;                    /* engine events not dispatched while handed over */
    bool fSuppressed = false, fHandover = false;
};

OSDefineMetaClassAndStructors(SnapI2CHIDBus, IOService)
OSDefineMetaClassAndStructors(SnapI2CHIDDevice, IOHIDDevice)
OSDefineMetaClassAndStructors(SnapI2CHIDMultitouch, IOHIDDevice)
OSDefineMetaClassAndStructors(SnapI2CHIDKeyboard, IOHIDEventService)
OSDefineMetaClassAndStructors(SnapI2CHIDPointer, IOHIDEventService)

#include "SnapI2CHID/bus.cpp"

#include "SnapI2CHID/ptp.cpp"

#include "SnapI2CHID/publish.cpp"

#include "SnapI2CHID/device.cpp"

#include "SnapI2CHID/multitouch.cpp"

#include "SnapI2CHID/keyboard.cpp"

#include "SnapI2CHID/pointer.cpp"
