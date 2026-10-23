/* SPDX-License-Identifier: MIT */
/* SnapI2CHIDKeyboard, the keyboard's IOHIDEventService. Part of SnapI2CHID.cpp's translation unit, included there
 * in order; not compiled on its own. */

/* ---------------------------------------------------------- keyboard ---- */

bool SnapI2CHIDKeyboard::initWithBus(SnapI2CHIDBus *bus)
{
    OSDictionary *props = OSDictionary::withCapacity(2);
    bool ok;

    if (!props)
        return false;
    props->setObject(kIOHIDBuiltInKey, kOSBooleanTrue);
    /* As Apple's keyboard personalities carry it for the HID event system. */
    OSString *empty = OSString::withCString("");
    if (empty) {
        props->setObject("HIDDefaultBehavior", empty);
        empty->release();
    }
    ok = IOHIDEventService::init(props);
    props->release();
    if (!ok)
        return false;
    fBus = bus;
    bzero(&fState, sizeof(fState));
    fProductName = bus->copyProduct();
    return fProductName != nullptr;
}

void SnapI2CHIDKeyboard::free()
{
    OSSafeReleaseNULL(fProductName);
    IOHIDEventService::free();
}

IOWorkLoop *SnapI2CHIDKeyboard::getWorkLoop() const
{
    return fBus && fBus->fWorkLoop ? fBus->fWorkLoop : IOHIDEventService::getWorkLoop();
}

bool SnapI2CHIDKeyboard::handleStart(IOService *provider)
{
    return provider == fBus && IOHIDEventService::handleStart(provider);
}

OSString *SnapI2CHIDKeyboard::getTransport() { return OSString::withCString("I2C"); }
UInt32 SnapI2CHIDKeyboard::getLocationID() { return fBus->location(); }
UInt32 SnapI2CHIDKeyboard::getVendorID() { return fBus->device()->desc.vendor_id; }
UInt32 SnapI2CHIDKeyboard::getProductID() { return fBus->device()->desc.product_id; }
UInt32 SnapI2CHIDKeyboard::getVersion() { return fBus->device()->desc.device_version; }
/* HID country code 33, US: the A16's ANSI layout (the keyboardtype preference is keyed by it). */
UInt32 SnapI2CHIDKeyboard::getCountryCode() { return 33; }
OSString *SnapI2CHIDKeyboard::getManufacturer() { return OSString::withCString("ASUSTeK"); }
OSString *SnapI2CHIDKeyboard::getProduct()
{
    fProductName->retain();
    return fProductName;
}
UInt32 SnapI2CHIDKeyboard::getReportInterval() { return fBus->device()->cfg.poll_us; }
UInt32 SnapI2CHIDKeyboard::getPrimaryUsagePage() { return 0x01; }  /* Generic Desktop */
UInt32 SnapI2CHIDKeyboard::getPrimaryUsage() { return 0x06; }      /* Keyboard */

void SnapI2CHIDKeyboard::emit(void *ctx, uint32_t page, uint32_t usage, uint32_t down)
{
    SnapI2CHIDKeyboard *self = static_cast<SnapI2CHIDKeyboard *>(ctx);
    self->fTransitions++;
    if (down && page == HID_KBD_PAGE_KEYBOARD && (usage < 0xe0u || usage > 0xe7u))   /* not a modifier */
        __atomic_store_n(&gSnapLastKeyDownUs, uptimeUs(self->fNow), __ATOMIC_RELAXED);
    self->dispatchKeyboardEvent(self->fNow, page, usage, down);
}

void SnapI2CHIDKeyboard::handleReport(const uint8_t *report, uint32_t len)
{
    uint32_t before = fTransitions;
    int r;
    if (fSuppressed) {
        /* D1: counts only, while the event driver that opened the device dispatches them. */
        if (++fYieldedReports >= fNextYieldLog) {
            IOLog(LOG_PREFIX "keyboard yielded: %u reports left to the event driver\n", fYieldedReports);
            fNextYieldLog = fNextYieldLog < 256u ? fNextYieldLog * 16u : fNextYieldLog + 4096u;
        }
        return;
    }
    clock_get_uptime(&fNow);
    fReports++;
    r = hid_kbd_translate(&fState, report, len, emit, this);
    /* Counts only: the user may be typing a password. */
    if (fTransitions != before && fTransitions >= fNextLog) {
        IOLog(LOG_PREFIX "keyboard dispatched %u key transitions from %u reports\n", fTransitions, fReports);
        fNextLog = fNextLog < 64u ? fNextLog * 4u : fNextLog + 1024u;
    } else if (r == HID_KBD_BAD_LENGTH || r == HID_KBD_ROLLOVER_REPORT) {
        if (fReports <= 64u)
            IOLog(LOG_PREFIX "keyboard report %u not taken (%s), id %u, %u bytes\n", fReports,
                  r == HID_KBD_BAD_LENGTH ? "length" : "rollover", len ? report[0] : 0u, len);
    }
}

void SnapI2CHIDKeyboard::releaseKeys()
{
    clock_get_uptime(&fNow);
    hid_kbd_release_all(&fState, emit, this);
}

void SnapI2CHIDKeyboard::suppress(const char *why)
{
    if (fSuppressed)
        return;
    IOLog(LOG_PREFIX "keyboard event service stops dispatching: %s opened the HID device\n", why);
    clock_get_uptime(&fNow);
    hid_kbd_release_all(&fState, emit, this);
    fSuppressed = true;
}

/* Y3: the event driver closed the device (its dext exited): keys come from us again, from a clean
 * state (everything was released when we yielded). */
void SnapI2CHIDKeyboard::resume()
{
    if (!fSuppressed)
        return;
    IOLog(LOG_PREFIX "keyboard event service dispatches again after %u yielded reports\n", fYieldedReports);
    bzero(&fState, sizeof(fState));
    fSuppressed = false;
}

/* LEDs (page 8): Num 1, Caps 2, Scroll 3, Compose 4, Kana 5 -> output report 54, one bit each. */
IOReturn SnapI2CHIDKeyboard::setElementValue(UInt32 usagePage, UInt32 usage, UInt32 value)
{
    uint8_t buf[2];
    IOReturn r;
    if (usagePage != 0x08 || usage < 1 || usage > 5)
        return IOHIDEventService::setElementValue(usagePage, usage, value);
    fLeds = value ? fLeds | (1u << (usage - 1)) : fLeds & ~(1u << (usage - 1));
    buf[0] = HID_KBD_REPORT_KEYS;
    buf[1] = (uint8_t)fLeds;
    r = fBus->request(SnapI2CHIDBus::kRequestOutput, I2C_HID_TYPE_OUTPUT, HID_KBD_REPORT_KEYS, buf, 2, nullptr);
    if (++fLedRequests <= 64 || !(fLedRequests & 63))
        IOLog(LOG_PREFIX "keyboard LED %u: usage %u -> %u, output 0x%02x -> 0x%x\n", fLedRequests,
              (unsigned)usage, (unsigned)value, fLeds, r);
    return r;
}
