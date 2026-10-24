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
#if SNAP_I2C_HID_FNKEYS
    startFnKeys();
#endif
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
#if SNAP_I2C_HID_FNKEYS
        /* The event driver dispatches reports 54 and 55 but nothing of report 90: page 0xFF31 is no usage a
         * generic HID driver handles. So the Fn-key layer still dispatches report 90 (fFn.yielded). */
        if (fFn.flags) {
            clock_get_uptime(&fNow);
            (void)hid_fnkeys_translate(&fFn, &fState, report, len, emit, this, note, this);
        }
#endif
        /* D1: counts only, while the event driver that opened the device dispatches them. */
        if (++fYieldedReports >= fNextYieldLog) {
            IOLog(LOG_PREFIX "keyboard yielded: %u reports left to the event driver\n", fYieldedReports);
            fNextYieldLog = fNextYieldLog < 256u ? fNextYieldLog * 16u : fNextYieldLog + 4096u;
        }
        return;
    }
    clock_get_uptime(&fNow);
    fReports++;
#if SNAP_I2C_HID_FNKEYS
    r = fFn.flags ? hid_fnkeys_translate(&fFn, &fState, report, len, emit, this, note, this)
                  : hid_kbd_translate(&fState, report, len, emit, this);
#else
    r = hid_kbd_translate(&fState, report, len, emit, this);
#endif
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
#if SNAP_I2C_HID_FNKEYS
    if (fFn.flags) {
        hid_fnkeys_release_all(&fFn, &fState, emit, this);
        return;
    }
#endif
    hid_kbd_release_all(&fState, emit, this);
}

void SnapI2CHIDKeyboard::suppress(const char *why)
{
    if (fSuppressed)
        return;
    IOLog(LOG_PREFIX "keyboard event service stops dispatching: %s opened the HID device\n", why);
    clock_get_uptime(&fNow);
#if SNAP_I2C_HID_FNKEYS
    if (fFn.flags) {
        /* What reports 54 and 55 hold is released as it was dispatched (a hotkey as its macOS usage);
         * report 90's codes stay ours. */
        hid_fnkeys_release_keyboard(&fFn, &fState, emit, this);
        fFn.yielded = 1;
        fSuppressed = true;
        return;
    }
#endif
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
#if SNAP_I2C_HID_FNKEYS
    fFn.yielded = 0;
#endif
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

#if SNAP_I2C_HID_FNKEYS
/* ------------------------------------------------------- Fn-key layer ---- */

/* snap_i2c_fnkeys (hid-keyboard.h HID_FNKEYS_*): absent or 0 leaves the keyboard exactly as without the layer; a
 * set hid_fnkeys_init refuses (unknown bits, brightness encoding 3, host lock without translate) leaves it off as
 * well and says why. */
void SnapI2CHIDKeyboard::startFnKeys()
{
    uint32_t flags = 0;
    const bool given = PE_parse_boot_argn("snap_i2c_fnkeys", &flags, sizeof(flags));
    const int r = hid_fnkeys_init(&fFn, given ? flags : 0u);
    const uint32_t declared = hid_rdesc_report_bytes(&fBus->device()->rinfo, HID_REPORT_INPUT, HID_KBD_REPORT_VENDOR);
    uint16_t downPage, downUsage, upPage, upUsage;

    bzero(&fRate, sizeof(fRate));
    if (!given) {
        IOLog(LOG_PREFIX "fn keys: off (no snap_i2c_fnkeys boot-arg)\n");
        return;
    }
    if (r != HID_FNKEYS_OK) {
        IOLog(LOG_PREFIX "fn keys: snap_i2c_fnkeys 0x%x refused (%s); off\n", flags, hid_fnkeys_init_name(r));
        return;
    }
    if (!fFn.flags) {
        IOLog(LOG_PREFIX "fn keys: off (snap_i2c_fnkeys 0x%x)\n", flags);
        return;
    }
    hid_fnkeys_brightness(&fFn, 0, &downPage, &downUsage);
    hid_fnkeys_brightness(&fFn, 1, &upPage, &upUsage);
    IOLog(LOG_PREFIX "fn keys: snap_i2c_fnkeys 0x%x: translate %s, raw log %s, host Fn-lock %s; brightness down "
          "0x%04x/0x%04x, up 0x%04x/0x%04x; input report 90 %u bytes in the descriptor%s\n", flags,
          (flags & HID_FNKEYS_TRANSLATE) ? "on" : "off", (flags & HID_FNKEYS_RAW_LOG) ? "on" : "off",
          (flags & HID_FNKEYS_HOST_LOCK) ? "on" : "off", downPage, downUsage, upPage, upUsage, declared,
          declared == HID_KBD_VENDOR_BYTES ? "" : " (not the A16's 6: its reports are refused by length)");
}

/* The raw log's rate limit (hid_fnkeys_rate_allow): what it dropped is reported with the next line it allows, and
 * the lifetime cap once. */
bool SnapI2CHIDKeyboard::mayLog()
{
    uint64_t now;
    clock_get_uptime(&now);
    if (!hid_fnkeys_rate_allow(&fRate, uptimeUs(now)))
        return false;
    if (fRate.dropped) {
        IOLog(LOG_PREFIX "fn keys: %u raw-log lines dropped by the rate limit\n", fRate.dropped);
        fRate.dropped = 0;
    }
    if (fRate.lines == HID_FNKEYS_RATE_LIFETIME)
        IOLog(LOG_PREFIX "fn keys: the raw log reached %u lines; no more this boot\n", HID_FNKEYS_RATE_LIFETIME);
    return true;
}

static void hexBytes(char *out, size_t cap, const uint8_t *p, uint32_t n)
{
    static const char digits[] = "0123456789abcdef";
    size_t at = 0;
    for (uint32_t i = 0; i < n && at + 4 <= cap; i++) {
        if (i)
            out[at++] = ' ';
        out[at++] = digits[p[i] >> 4];
        out[at++] = digits[p[i] & 15];
    }
    out[at < cap ? at : cap - 1] = 0;
}

void SnapI2CHIDKeyboard::note(void *ctx, const struct hid_fnkeys_note *n)
{
    SnapI2CHIDKeyboard *self = static_cast<SnapI2CHIDKeyboard *>(ctx);
    char bytes[3 * 16 + 1], out[48];
    const bool driver = self->fFn.yielded && (n->kind == HID_FNKEYS_NOTE_KEY || n->kind == HID_FNKEYS_NOTE_CONSUMER);

    if (!self->mayLog())
        return;
    hexBytes(bytes, sizeof(bytes), n->raw, n->raw ? (n->raw_len < 16u ? n->raw_len : 16u) : 0u);
    if (driver)
        snprintf(out, sizeof(out), "-> the event driver");
    else if (n->out_page || n->out_usage)
        snprintf(out, sizeof(out), "-> 0x%04x/0x%04x", n->out_page, n->out_usage);
    else
        snprintf(out, sizeof(out), "-> nothing");
    switch (n->kind) {
    case HID_FNKEYS_NOTE_VENDOR:
        IOLog(LOG_PREFIX "fn keys: report 90 [%s]: code 0x%02x %s %s\n", bytes, n->code, n->down ? "down" : "up", out);
        break;
    case HID_FNKEYS_NOTE_CONSUMER:
        IOLog(LOG_PREFIX "fn keys: report 55 usage 0x%04x %s %s\n", n->code, n->down ? "down" : "up", out);
        break;
    case HID_FNKEYS_NOTE_KEY:
        IOLog(LOG_PREFIX "fn keys: report 54 key 0x%02x %s, modifiers 0x%02x %s\n", n->code, n->down ? "down" : "up",
              n->mods, out);
        break;
    case HID_FNKEYS_NOTE_RANGE:
        IOLog(LOG_PREFIX "fn keys: report 54 key 0x%02x %s past the logical maximum 0x65, modifiers 0x%02x: not "
              "dispatched\n", n->code, n->down ? "down" : "up", n->mods);
        break;
    case HID_FNKEYS_NOTE_CHORD:
        IOLog(LOG_PREFIX "fn keys: report 54 chord: modifiers 0x%02x and key 0x%02x went down in one report\n",
              n->mods, n->code);
        break;
    case HID_FNKEYS_NOTE_REPORT:
        IOLog(LOG_PREFIX "fn keys: report %u, %u bytes [%s]: not taken\n", n->id, n->code, bytes);
        break;
    case HID_FNKEYS_NOTE_LOCK:
        IOLog(LOG_PREFIX "fn keys: host Fn-lock %s\n", n->down ? "on: F1-F12 first, hotkeys with Fn"
                                                               : "off: hotkeys first, F1-F12 with Fn");
        break;
    default:
        break;
    }
}

/* bus.cpp noteRejectedInput: an input the transport refused (an undeclared ID, or a declared one at another size)
 * never reaches handleReport. input is the raw read, length prefix first; report 54 is named by size only. */
void SnapI2CHIDKeyboard::noteRejected(const uint8_t *input, uint32_t len)
{
    char bytes[3 * 16 + 1];
    if (!rawLog() || len < 3u || !mayLog())
        return;
    const uint32_t prefix = (uint32_t)input[0] | (uint32_t)input[1] << 8;
    const uint32_t body = prefix >= 3u && prefix - 2u <= len - 2u ? prefix - 2u : 1u;
    if (input[2] == HID_KBD_REPORT_KEYS) {
        IOLog(LOG_PREFIX "fn keys: transport refused an input: length prefix %u, report 54 (bytes withheld)\n", prefix);
        return;
    }
    hexBytes(bytes, sizeof(bytes), input + 2, body < 16u ? body : 16u);
    IOLog(LOG_PREFIX "fn keys: transport refused an input: length prefix %u, report %u [%s]\n", prefix, input[2], bytes);
}
#endif
