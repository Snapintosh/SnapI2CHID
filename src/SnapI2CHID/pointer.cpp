/* SPDX-License-Identifier: MIT */
/* SnapI2CHIDPointer, the touchpad's IOHIDEventService in mouse and Precision Touchpad mode. Part of SnapI2CHID.cpp's
 * translation unit, included there in order; not compiled on its own. */

/* ----------------------------------------------------------- pointer ---- */

static void setNumber(OSDictionary *d, const char *key, uint64_t value)
{
    OSNumber *n = OSNumber::withNumber(value, 32);
    if (n) {
        d->setObject(key, n);
        n->release();
    }
}

/* The scroll acceleration Apple's built-in trackpad service publishes on 26A428
 * (AppleMultitouchTrackpadHIDEventDriver, HIDScrollAccelCurves; docs/A16-INPUT-PTP-20261020.md
 * section 2.3), for deltas in 1/400 inch at HIDScrollResolution 400 dpi. GainParabolic 0 = absent. */
static const struct {
    uint32_t index, gain_linear, gain_parabolic, tangent_linear, tangent_parabolic_root;
} kTrackpadScrollCurves[10] = {
    {0, 65536, 0, 393216, 786432},           {8192, 62259, 39322, 406323, 786432},
    {32768, 58982, 58982, 419430, 786432},   {45056, 55706, 78643, 432538, 786432},
    {57344, 52429, 91750, 445645, 786432},   {65536, 49152, 104858, 458752, 786432},
    {98304, 45875, 117965, 471859, 786432},  {131072, 42598, 131072, 484966, 786432},
    {163840, 39322, 144179, 498074, 786432}, {196608, 36045, 157286, 511181, 786432},
};

/* Section 4.4: the trackpad scroll identity, only in touchpad mode. Pointer acceleration keys
 * stay unset, so one finger takes the same mouse acceleration path (and user setting) as today. */
static void addTrackpadScrollIdentity(OSDictionary *props)
{
    OSString *type = OSString::withCString("HIDTrackpadScrollAcceleration");
    OSArray *curves = OSArray::withCapacity(10);
    if (type) {
        props->setObject("HIDScrollAccelerationType", type);
        type->release();
    }
    setNumber(props, "HIDScrollResolution", (uint64_t)HID_PTP_UNITS_PER_INCH << 16);
    setNumber(props, "HIDTrackpadScrollAcceleration", 20480);   /* 26A428's default; the event system pushes the user's */
    for (uint32_t i = 0; curves && i < 10; i++) {
        OSDictionary *point = OSDictionary::withCapacity(5);
        if (!point)
            continue;
        setNumber(point, "HIDAccelIndex", kTrackpadScrollCurves[i].index);
        setNumber(point, "HIDAccelGainLinear", kTrackpadScrollCurves[i].gain_linear);
        if (kTrackpadScrollCurves[i].gain_parabolic)
            setNumber(point, "HIDAccelGainParabolic", kTrackpadScrollCurves[i].gain_parabolic);
        setNumber(point, "HIDAccelTangentSpeedLinear", kTrackpadScrollCurves[i].tangent_linear);
        setNumber(point, "HIDAccelTangentSpeedParabolicRoot", kTrackpadScrollCurves[i].tangent_parabolic_root);
        curves->setObject(point);
        point->release();
    }
    if (curves) {
        props->setObject("HIDScrollAccelCurves", curves);
        curves->release();
    }
}

bool SnapI2CHIDPointer::initWithBus(SnapI2CHIDBus *bus, const struct hid_pointer_layout *layout,
                                    const struct hid_ptp_layout *ptp)
{
    OSDictionary *props;
    bool ok;

    if (!layout && !ptp)
        return false;
    props = OSDictionary::withCapacity(ptp ? 6 : 2);
    if (!props)
        return false;
    props->setObject(kIOHIDBuiltInKey, kOSBooleanTrue);
    OSString *empty = OSString::withCString("");
    if (empty) {
        props->setObject("HIDDefaultBehavior", empty);
        empty->release();
    }
    if (ptp)
        addTrackpadScrollIdentity(props);
    ok = IOHIDEventService::init(props);
    props->release();
    if (!ok)
        return false;
    fBus = bus;
    bzero(&fLayout, sizeof(fLayout));
    fHaveMouse = layout != nullptr;
    if (layout)
        fLayout = *layout;
    bzero(&fState, sizeof(fState));
    fPtpArmed = ptp != nullptr;
    if (ptp)
        hid_ptp_engine_init(&fEngine, ptp);
    fSink.ctx = this;
    fSink.pointer = ptpPointer;
    fSink.scroll = ptpScroll;
    fSink.frame = ptpFrame;
    fProductName = bus->copyProduct();
    return fProductName != nullptr;
}

void SnapI2CHIDPointer::free()
{
    OSSafeReleaseNULL(fProductName);
    IOHIDEventService::free();
}

IOWorkLoop *SnapI2CHIDPointer::getWorkLoop() const
{
    return fBus && fBus->fWorkLoop ? fBus->fWorkLoop : IOHIDEventService::getWorkLoop();
}

bool SnapI2CHIDPointer::handleStart(IOService *provider)
{
    return provider == fBus && IOHIDEventService::handleStart(provider);
}

OSString *SnapI2CHIDPointer::getTransport() { return OSString::withCString("I2C"); }
UInt32 SnapI2CHIDPointer::getLocationID() { return fBus->location(); }
UInt32 SnapI2CHIDPointer::getVendorID() { return fBus->device()->desc.vendor_id; }
UInt32 SnapI2CHIDPointer::getProductID() { return fBus->device()->desc.product_id; }
UInt32 SnapI2CHIDPointer::getVersion() { return fBus->device()->desc.device_version; }
UInt32 SnapI2CHIDPointer::getCountryCode() { return 0; }
OSString *SnapI2CHIDPointer::getManufacturer() { return OSString::withCString("ASUSTeK"); }
OSString *SnapI2CHIDPointer::getProduct()
{
    fProductName->retain();
    return fProductName;
}
UInt32 SnapI2CHIDPointer::getReportInterval() { return fBus->device()->cfg.poll_us; }
UInt32 SnapI2CHIDPointer::getPrimaryUsagePage() { return 0x01; }  /* Generic Desktop */
UInt32 SnapI2CHIDPointer::getPrimaryUsage() { return 0x02; }      /* Mouse */

void SnapI2CHIDPointer::noteEvent()
{
    uint32_t events = fMoves + fScrolls;
    if (events >= fNextLog) {
        IOLog(LOG_PREFIX "pointer dispatched %u moves and %u scrolls from %u reports\n", fMoves, fScrolls, fReports);
        fNextLog = fNextLog < 64u ? fNextLog * 4u : fNextLog + 1024u;
    }
}

void SnapI2CHIDPointer::move(void *ctx, int32_t dx, int32_t dy, uint32_t buttons)
{
    SnapI2CHIDPointer *self = static_cast<SnapI2CHIDPointer *>(ctx);
    self->fMoves++;
    self->dispatchRelativePointerEvent(self->fNow, dx, dy, buttons);
    self->noteEvent();
}

void SnapI2CHIDPointer::scroll(void *ctx, int32_t vertical, int32_t horizontal)
{
    SnapI2CHIDPointer *self = static_cast<SnapI2CHIDPointer *>(ctx);
    self->fScrolls++;
    self->dispatchScrollWheelEvent(self->fNow, vertical, horizontal, 0);
    self->noteEvent();
}

void SnapI2CHIDPointer::ptpPointer(void *ctx, int32_t dx, int32_t dy, uint32_t buttons)
{
    SnapI2CHIDPointer *self = static_cast<SnapI2CHIDPointer *>(ctx);
    if (self->fHandover) {                       /* Apple's multitouch stack has the touchpad */
        self->fHandedOver++;
        return;
    }
    self->fMoves++;
    self->dispatchRelativePointerEventWithFixed(self->fNow, dx, dy, buttons);
}

void SnapI2CHIDPointer::ptpScroll(void *ctx, int32_t vertical, int32_t horizontal, uint32_t options)
{
    SnapI2CHIDPointer *self = static_cast<SnapI2CHIDPointer *>(ctx);
    if (self->fHandover) {
        self->fHandedOver++;
        return;
    }
    self->fScrolls++;
    self->dispatchScrollWheelEventWithFixed(self->fNow, vertical, horizontal, 0, options);
}

/* D2: the first 8 complete frames: how many contacts, touching, confident (palm-free), the
 * buttons, and what the engine dispatched so far; never positions. */
void SnapI2CHIDPointer::ptpFrame(void *ctx, const struct hid_ptp_contact *c, uint32_t n, uint32_t buttons,
                                 uint32_t scan, int has_scan)
{
    SnapI2CHIDPointer *self = static_cast<SnapI2CHIDPointer *>(ctx);
    uint32_t tips = 0, confident = 0;
    if (self->fBus->fMT)                         /* every frame, to Apple's multitouch stack */
        self->fBus->fMT->frame(c, n, buttons, scan, has_scan);
    if (self->fLoggedFrames >= 8u)
        return;
    self->fLoggedFrames++;
    for (uint32_t i = 0; i < n; i++) {
        tips += c[i].tip != 0;
        confident += c[i].tip && c[i].confident;
    }
    IOLog(LOG_PREFIX "touchpad frame %u: %u contacts, %u touching, %u confident, buttons 0x%x; dispatched so far "
          "%u pointer, %u scroll\n", self->fLoggedFrames, n, tips, confident, buttons, self->fMoves, self->fScrolls);
}

/* Counts only, at 1, 16, 256 frames and then every 4096. */
void SnapI2CHIDPointer::notePtp(int result)
{
    const struct hid_ptp_stats *s = &fEngine.stats;
    if (result == HID_PTP_BAD_LENGTH && s->bad_length <= 8u)
        IOLog(LOG_PREFIX "touchpad report %u not taken (length)\n", s->reports);
    if (s->frames < fNextPtpLog)
        return;
    IOLog(LOG_PREFIX "touchpad: %u frames, %u pointer, %u scrolls (%u momentum), %u taps (%u secondary, %u while "
          "typing), %u palms, %u bad lengths, %u scan fallbacks, max %u fingers\n", s->frames, s->pointer_events,
          s->scrolls, s->momentum, s->taps, s->secondary_taps, s->typing_blocked_taps, s->palms, s->bad_length,
          s->scan_fallbacks, s->max_fingers);
    fNextPtpLog = fNextPtpLog < 256u ? fNextPtpLog * 16u : fNextPtpLog + 4096u;
}

void SnapI2CHIDPointer::handleReport(const uint8_t *report, uint32_t len)
{
    int r;
    if (!len)
        return;
    if (fSuppressed) {
        /* D1: counts only, while the event driver that opened the device dispatches them. */
        fYieldedReports++;
        if (fPtpArmed && report[0] == fEngine.l.report_id)
            fYieldedPtp++;
        if (fYieldedReports >= fNextYieldLog) {
            IOLog(LOG_PREFIX "pointer yielded: %u reports left to the event driver (%u touchpad frames)\n",
                  fYieldedReports, fYieldedPtp);
            fNextYieldLog = fNextYieldLog < 256u ? fNextYieldLog * 16u : fNextYieldLog + 4096u;
        }
        return;
    }
    clock_get_uptime(&fNow);
    fReports++;
    if (fPtpArmed && report[0] == fEngine.l.report_id) {
        r = hid_ptp_engine_report(&fEngine, report, len, uptimeUs(fNow),
                                  __atomic_load_n(&gSnapLastKeyDownUs, __ATOMIC_RELAXED), &fSink);
        notePtp(r);
        return;
    }
    if (!fHaveMouse)
        return;
    if (fPtpArmed && report[0] == fLayout.report_id) {
        ++fMouseWhileArmed;
        if (fMouseWhileArmed <= 4u)
            IOLog(LOG_PREFIX "pointer: mouse-mode report %u while in touchpad mode (%u)\n", report[0], fMouseWhileArmed);
        if (fMouseWhileArmed == 1u || fMouseWhileArmed == 64u || fMouseWhileArmed == 4096u)
            fBus->fPtpReapply = true;        /* F24: the next bus step applies the switch again */
    }
    r = hid_pointer_translate(&fLayout, &fState, report, len, move, scroll, this);
    if (r == HID_POINTER_BAD_LENGTH && fReports <= 64u)
        IOLog(LOG_PREFIX "pointer report %u not taken (length), id %u, %u bytes\n", fReports,
              len ? report[0] : 0u, len);
}

void SnapI2CHIDPointer::tick()
{
    if (!fPtpArmed || fSuppressed || !hid_ptp_engine_busy(&fEngine))
        return;
    clock_get_uptime(&fNow);
    hid_ptp_engine_tick(&fEngine, uptimeUs(fNow), &fSink);
}

/* Scroll and momentum ended, buttons released, contacts forgotten (F10, F22). */
void SnapI2CHIDPointer::resetTouch()
{
    if (!fPtpArmed)
        return;
    clock_get_uptime(&fNow);
    hid_ptp_engine_reset(&fEngine, uptimeUs(fNow), &fSink);
}

void SnapI2CHIDPointer::releaseButtons()
{
    resetTouch();
    if (!fState.buttons)
        return;
    clock_get_uptime(&fNow);
    dispatchRelativePointerEvent(fNow, 0, 0, 0);
    fState.buttons = 0;
}

void SnapI2CHIDPointer::suppress(const char *why)
{
    if (fSuppressed)
        return;
    IOLog(LOG_PREFIX "pointer event service stops dispatching: %s opened the HID device\n", why);
    releaseButtons();
    fSuppressed = true;
}

/* M1, M6, M7: hand the touchpad to Apple's multitouch stack while it has a frames consumer, take it
 * back when the consumer goes (WindowServer exited). Either way the engine's touch state is reset with
 * its events going where they went before (ending a scroll or a button we dispatched; dropping the
 * ones Apple's stack had), so nothing is left open on either side. */
void SnapI2CHIDPointer::handover(bool on)
{
    if (on == fHandover || !fPtpArmed)
        return;
    resetTouch();
    fHandover = on;
    IOLog(LOG_PREFIX "pointer %s the touchpad %s the multitouch stack (%u engine events not dispatched so far)\n",
          on ? "hands" : "takes back", on ? "to" : "from", fHandedOver);
}

/* Y3: the event driver closed the device: the pointer dispatches again from clean state. */
void SnapI2CHIDPointer::resume()
{
    if (!fSuppressed)
        return;
    IOLog(LOG_PREFIX "pointer event service dispatches again after %u yielded reports\n", fYieldedReports);
    bzero(&fState, sizeof(fState));
    fSuppressed = false;
    resetTouch();
}
