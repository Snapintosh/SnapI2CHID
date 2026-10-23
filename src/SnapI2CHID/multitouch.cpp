/* SPDX-License-Identifier: MIT */
/* SnapI2CHIDMultitouch, the armed touchpad as an Apple multitouch IOHIDDevice. Part of SnapI2CHID.cpp's translation
 * unit, included there in order; not compiled on its own. */

/* -------------------------------------------------------- multitouch ---- */

bool SnapI2CHIDMultitouch::initWithBus(SnapI2CHIDBus *bus, const struct hid_mt2 *geometry)
{
    OSDictionary *props = OSDictionary::withCapacity(2);
    bool ok;
    if (!props || !geometry || !geometry->wq)
        return false;
    props->setObject(kIOHIDBuiltInKey, kOSBooleanTrue);
    ok = IOHIDDevice::init(props);
    props->release();
    if (!ok)
        return false;
    fBus = bus;
    fMT = *geometry;
    fReport = IOBufferMemoryDescriptor::withCapacity(HID_MT2_MAX_REPORT, kIODirectionOutIn);
    return fReport != nullptr;
}

void SnapI2CHIDMultitouch::free()
{
    OSSafeReleaseNULL(fReport);
    IOHIDDevice::free();
}

bool SnapI2CHIDMultitouch::handleStart(IOService *provider)
{
    return provider == fBus && IOHIDDevice::handleStart(provider);
}

IOReturn SnapI2CHIDMultitouch::newReportDescriptor(IOMemoryDescriptor **descriptor) const
{
    uint32_t len;
    const uint8_t *bytes = hid_mt2_report_descriptor(&len);
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withBytes(bytes, len, kIODirectionOutIn);
    if (!md)
        return kIOReturnNoMemory;
    *descriptor = md;
    return kIOReturnSuccess;
}

OSString *SnapI2CHIDMultitouch::newTransportString() const { return OSString::withCString("I2C"); }
OSString *SnapI2CHIDMultitouch::newManufacturerString() const { return OSString::withCString("ASUSTeK"); }
OSString *SnapI2CHIDMultitouch::newProductString() const
{
    return OSString::withCString("Precision Touchpad (multitouch frames)");
}
OSNumber *SnapI2CHIDMultitouch::newVendorIDNumber() const { return OSNumber::withNumber(HID_MT2_VENDOR, 32); }
OSNumber *SnapI2CHIDMultitouch::newProductIDNumber() const { return OSNumber::withNumber(HID_MT2_PRODUCT, 32); }
OSNumber *SnapI2CHIDMultitouch::newVersionNumber() const { return OSNumber::withNumber(HID_MT2_VERSION, 32); }
OSNumber *SnapI2CHIDMultitouch::newLocationIDNumber() const { return OSNumber::withNumber(fBus->location(), 32); }
OSNumber *SnapI2CHIDMultitouch::newReportIntervalNumber() const
{
    return OSNumber::withNumber(fBus->device()->cfg.poll_us, 32);
}

/* M14: every request by type, report ID, length and result: 32, then every 64th. */
void SnapI2CHIDMultitouch::logRequest(const char *what, IOHIDReportType type, uint8_t id, uint32_t len, IOReturn result)
{
    if (++fRequests <= 32 || !(fRequests & 63))
        IOLog(LOG_PREFIX "multitouch %s %u: type %u id 0x%02x length %u -> 0x%x\n", what, fRequests, (unsigned)type,
              id, len, result);
}

/* M4: the handshake replies, from memory (hid_mt2_feature), with the buffer's length set to the
 * reply: the driver reads the length back to know how much it got. */
IOReturn SnapI2CHIDMultitouch::getReport(IOMemoryDescriptor *report, IOHIDReportType type, IOOptionBits options)
{
    uint8_t buf[HID_MT2_MAX_FEATURE];
    uint8_t id = (uint8_t)(options & 0xff);
    IOBufferMemoryDescriptor *b = OSDynamicCast(IOBufferMemoryDescriptor, report);
    uint32_t n = 0;
    IOReturn r;
    if (!report || type != kIOHIDReportTypeFeature) {
        r = kIOReturnUnsupported;
    } else if (!(n = hid_mt2_feature(&fMT, id, __atomic_load_n(&fQuery, __ATOMIC_RELAXED), buf, sizeof(buf)))) {
        r = kIOReturnUnsupported;
    } else if (b ? n > b->getCapacity() : n > report->getLength()) {
        r = kIOReturnNoSpace;
    } else {
        if (b)
            b->setLength(n);
        report->writeBytes(0, buf, n);
        r = kIOReturnSuccess;
    }
    logRequest("getReport", type, id, n, r);
    return r;
}

/* M14: accepted and ignored (the touchpad is not an Apple sensor), except report 1's query. */
IOReturn SnapI2CHIDMultitouch::setReport(IOMemoryDescriptor *report, IOHIDReportType type, IOOptionBits options)
{
    uint8_t id = (uint8_t)(options & 0xff), q[2] = {0, 0};
    uint32_t len = report ? (uint32_t)report->getLength() : 0;
    if (type == kIOHIDReportTypeFeature && id == 0x01 && len >= 2) {
        report->readBytes(0, q, 2);
        __atomic_store_n(&fQuery, q[0] == 0x01 ? q[1] : q[0], __ATOMIC_RELAXED);
    }
    logRequest("setReport", type, id, len, kIOReturnSuccess);
    return kIOReturnSuccess;
}

void SnapI2CHIDMultitouch::send(uint32_t len)
{
    AbsoluteTime now;
    IOReturn r;
    if (!len || len > fReport->getCapacity())
        return;
    bcopy(fBuf, fReport->getBytesNoCopy(), len);
    fReport->setLength(len);
    clock_get_uptime(&now);
    r = handleReportWithTime(now, fReport, kIOHIDReportTypeInput, 0);
    fSent++;
    if (r != kIOReturnSuccess && (++fFailures <= 8 || !(fFailures & 255)))
        IOLog(LOG_PREFIX "multitouch report %u, %u bytes -> 0x%x (failure %u)\n", fSent, len, r, fFailures);
    noteFrames();
}

/* Counts only (never positions), at 1, 16, 256 reports and then every 4096. */
void SnapI2CHIDMultitouch::noteFrames()
{
    const struct hid_mt2_stats *s = &fMT.stats;
    if (fSent < fNextLog)
        return;
    IOLog(LOG_PREFIX "multitouch: %u reports (%u from frames, %u from ticks, %u empty), %u touches, %u palms, %u lifts "
          "(%u vanished), %u jumps, %u dropped, %u duplicate ids, max %u contacts, %u time fallbacks, %u failures\n",
          fSent, s->frames, s->tick_frames, s->empty_frames, s->touches, s->palms, s->lifts, s->vanished, s->jumps,
          s->dropped, s->duplicate_ids, s->max_contacts, s->time_fallbacks, fFailures);
    fNextLog = fNextLog < 256u ? fNextLog * 16u : fNextLog + 4096u;
}

void SnapI2CHIDMultitouch::frame(const struct hid_ptp_contact *c, uint32_t n, uint32_t buttons, uint32_t scan,
                                 int has_scan)
{
    AbsoluteTime now;
    clock_get_uptime(&now);
    send(hid_mt2_frame(&fMT, c, n, buttons, scan, has_scan, uptimeUs(now), fBuf, sizeof(fBuf)));
}

void SnapI2CHIDMultitouch::tick()
{
    AbsoluteTime now;
    if (!hid_mt2_busy(&fMT))
        return;
    clock_get_uptime(&now);
    send(hid_mt2_tick(&fMT, uptimeUs(now), fBuf, sizeof(fBuf)));
}

void SnapI2CHIDMultitouch::reset()
{
    hid_mt2_reset(&fMT);
}

void SnapI2CHIDMultitouch::kick()
{
    AbsoluteTime now;
    clock_get_uptime(&now);
    send(hid_mt2_empty(&fMT, uptimeUs(now), fBuf, sizeof(fBuf)));
}

/* The service plane above this device: IOHIDInterface, its driver (Apple's multitouch event
 * drivers are AppleMultitouch*), that driver's AppleMultitouchDevice and the user clients reading
 * its frames (AppleMultitouchDeviceUserClient: WindowServer on a 26A428 host). By class name: the
 * classes are Apple's, not linked. */
uint32_t SnapI2CHIDMultitouch::stack() const
{
    uint32_t bits = 0;
    OSIterator *interfaces = getChildIterator(gIOServicePlane);
    for (OSObject *o = interfaces ? interfaces->getNextObject() : nullptr; o; o = interfaces->getNextObject()) {
        IORegistryEntry *interface = OSDynamicCast(IORegistryEntry, o);
        OSIterator *drivers = interface ? interface->getChildIterator(gIOServicePlane) : nullptr;
        if (!interface)
            continue;
        bits |= kStackInterface;
        for (OSObject *d = drivers ? drivers->getNextObject() : nullptr; d; d = drivers->getNextObject()) {
            IORegistryEntry *driver = OSDynamicCast(IORegistryEntry, d);
            OSIterator *devices;
            if (!driver)
                continue;
            if (strncmp(driver->getMetaClass()->getClassName(), "AppleMultitouch", 15) != 0) {
                bits |= kStackOtherDriver;
                continue;
            }
            bits |= kStackAppleDriver;
            devices = driver->getChildIterator(gIOServicePlane);
            for (OSObject *m = devices ? devices->getNextObject() : nullptr; m; m = devices->getNextObject()) {
                IORegistryEntry *amd = OSDynamicCast(IORegistryEntry, m);
                OSIterator *clients;
                if (!amd || strcmp(amd->getMetaClass()->getClassName(), "AppleMultitouchDevice") != 0)
                    continue;
                bits |= kStackDevice;
                clients = amd->getChildIterator(gIOServicePlane);
                for (OSObject *u = clients ? clients->getNextObject() : nullptr; u; u = clients->getNextObject()) {
                    IORegistryEntry *client = OSDynamicCast(IORegistryEntry, u);
                    if (client && !strcmp(client->getMetaClass()->getClassName(), "AppleMultitouchDeviceUserClient"))
                        bits |= kStackConsumer;
                }
                OSSafeReleaseNULL(clients);
            }
            OSSafeReleaseNULL(devices);
        }
        OSSafeReleaseNULL(drivers);
    }
    OSSafeReleaseNULL(interfaces);
    return bits;
}
