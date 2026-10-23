/* SPDX-License-Identifier: MIT */
/* SnapI2CHIDDevice, the device's own IOHIDDevice (an armed Precision Touchpad gets none). Part of SnapI2CHID.cpp's
 * translation unit, included there in order; not compiled on its own. */

/* ------------------------------------------------------------ device ---- */

bool SnapI2CHIDDevice::initWithBus(SnapI2CHIDBus *bus)
{
    const struct i2c_hid_dev *dev = bus->device();
    OSDictionary *props = OSDictionary::withCapacity(2);
    bool ok;

    if (!props)
        return false;
    props->setObject(kIOHIDBuiltInKey, kOSBooleanTrue);
    ok = IOHIDDevice::init(props);
    props->release();
    if (!ok)
        return false;
    fBus = bus;
    fDescriptor = OSData::withBytes(dev->rdesc, dev->rdesc_len);
    fProduct = bus->copyProduct();
    fReport = IOBufferMemoryDescriptor::withCapacity(dev->desc.max_input, kIODirectionOutIn);
    fVendor = dev->desc.vendor_id;
    fProductID = dev->desc.product_id;
    fVersion = dev->desc.device_version;
    fInterval = dev->cfg.poll_us;
    return fDescriptor && fProduct && fReport;
}

void SnapI2CHIDDevice::free()
{
    OSSafeReleaseNULL(fDescriptor);
    OSSafeReleaseNULL(fProduct);
    OSSafeReleaseNULL(fReport);
    IOHIDDevice::free();
}

bool SnapI2CHIDDevice::handleStart(IOService *provider)
{
    return provider == fBus && IOHIDDevice::handleStart(provider);
}

IOReturn SnapI2CHIDDevice::newReportDescriptor(IOMemoryDescriptor **descriptor) const
{
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withBytes(
        fDescriptor->getBytesNoCopy(), fDescriptor->getLength(), kIODirectionOutIn);
    if (!md)
        return kIOReturnNoMemory;
    *descriptor = md;
    return kIOReturnSuccess;
}

OSString *SnapI2CHIDDevice::newTransportString() const
{
    return OSString::withCString("I2C");
}

OSString *SnapI2CHIDDevice::newProductString() const
{
    fProduct->retain();
    return fProduct;
}

OSNumber *SnapI2CHIDDevice::newVendorIDNumber() const
{
    return OSNumber::withNumber(fVendor, 32);
}

OSNumber *SnapI2CHIDDevice::newProductIDNumber() const
{
    return OSNumber::withNumber(fProductID, 32);
}

OSNumber *SnapI2CHIDDevice::newVersionNumber() const
{
    return OSNumber::withNumber(fVersion, 32);
}

OSNumber *SnapI2CHIDDevice::newLocationIDNumber() const
{
    return OSNumber::withNumber(fBus->location(), 32);
}

OSNumber *SnapI2CHIDDevice::newReportIntervalNumber() const
{
    return OSNumber::withNumber(fInterval, 32);
}

void SnapI2CHIDDevice::deliverReport(const uint8_t *report, uint32_t len)
{
    uint64_t now;
    if (len > fReport->getCapacity())
        return;
    IOReturn r;
    bcopy(report, fReport->getBytesNoCopy(), len);
    fReport->setLength(len);
    clock_get_uptime(&now);
    r = handleReportWithTime(now, fReport, kIOHIDReportTypeInput, 0);
    /* The first 16 by ID and size (never contents), then failures only, rate-limited. */
    if (++fReportCalls <= 16)
        IOLog(LOG_PREFIX "handleReportWithTime %u: id %u, %u bytes -> 0x%x\n", fReportCalls,
              len ? report[0] : 0u, len, r);
    else if (r != kIOReturnSuccess && (++fReportFailures <= 8 || !(fReportFailures & 255)))
        IOLog(LOG_PREFIX "handleReportWithTime %u: id %u, %u bytes -> 0x%x (failure %u)\n",
              fReportCalls, len ? report[0] : 0u, len, r, fReportFailures);
}

/* Every set/get request from the HID stack (a Caps Lock LED output report shows that
 * the event system turned a key into an event); after 64, every 64th. */
void SnapI2CHIDDevice::logRequest(const char *what, IOHIDReportType type, IOOptionBits options,
                                  IOByteCount len, IOReturn result)
{
    if (++fRequests <= 64 || !(fRequests & 63))
        IOLog(LOG_PREFIX "%s %u: type %u id %u length %u -> 0x%x\n", what, fRequests, (unsigned)type,
              (unsigned)(options & 0xff), (unsigned)len, result);
}

static uint32_t numberProperty(IORegistryEntry *entry, const char *key)
{
    OSNumber *n = OSDynamicCast(OSNumber, entry->getProperty(key));
    return n ? n->unsigned32BitValue() : 0xffffffffu;
}

static void logEntry(IORegistryEntry *entry, unsigned depth)
{
    IOService *service = OSDynamicCast(IOService, entry);
    IOLog(LOG_PREFIX "  %*s%s: state 0x%x%s\n", (int)(2 * depth), "", entry->getMetaClass()->getClassName(),
          service ? (unsigned)service->getState() : 0u, service && service->isOpen() ? " open" : "");
    if (depth >= 3)
        return;
    OSIterator *it = entry->getChildIterator(gIOServicePlane);
    if (!it)
        return;
    unsigned n = 0;
    for (OSObject *o = it->getNextObject(); o && n < 16; o = it->getNextObject(), n++) {
        IORegistryEntry *child = OSDynamicCast(IORegistryEntry, o);
        if (child)
            logEntry(child, depth + 1);
    }
    it->release();
}

/* The device's published matching properties, and the service-plane clients above it
 * (IOHIDInterface, then IOHIDEventDriver or AppleUserHIDEventService, then their clients),
 * with IOService state bits (2 registered, 4 matched) and whether each is open. Diagnostics
 * only: who dispatches is decided by handleOpen/handleClose (Y1). */
void SnapI2CHIDDevice::logRegistry(const char *when)
{
    OSString *transport = OSDynamicCast(OSString, getProperty(kIOHIDTransportKey));
    OSArray *pairs = OSDynamicCast(OSArray, getProperty(kIOHIDDeviceUsagePairsKey));
    char list[96];
    unsigned at = 0;
    list[0] = 0;
    for (unsigned i = 0; pairs && i < pairs->getCount() && at + 16 < sizeof(list); i++) {
        OSDictionary *pair = OSDynamicCast(OSDictionary, pairs->getObject(i));
        OSNumber *page = pair ? OSDynamicCast(OSNumber, pair->getObject(kIOHIDDeviceUsagePageKey)) : nullptr;
        OSNumber *usage = pair ? OSDynamicCast(OSNumber, pair->getObject(kIOHIDDeviceUsageKey)) : nullptr;
        at += (unsigned)snprintf(list + at, sizeof(list) - at, "%s%u/%u", i ? " " : "",
                                 page ? page->unsigned32BitValue() : 0u, usage ? usage->unsigned32BitValue() : 0u);
    }
    IOLog(LOG_PREFIX "HID stack at %s: primary %u/%u pairs [%s] transport %s built-in %s vendor 0x%x product 0x%x "
          "location 0x%x country %d interval %u max-in %u max-out %u\n", when,
          numberProperty(this, kIOHIDPrimaryUsagePageKey), numberProperty(this, kIOHIDPrimaryUsageKey), list,
          transport ? transport->getCStringNoCopy() : "-",
          getProperty(kIOHIDBuiltInKey) == kOSBooleanTrue ? "yes" : "no",
          numberProperty(this, kIOHIDVendorIDKey), numberProperty(this, kIOHIDProductIDKey),
          numberProperty(this, kIOHIDLocationIDKey), (int)numberProperty(this, kIOHIDCountryCodeKey),
          numberProperty(this, kIOHIDReportIntervalKey), numberProperty(this, kIOHIDMaxInputReportSizeKey),
          numberProperty(this, kIOHIDMaxOutputReportSizeKey));
    logEntry(this, 0);
}

/* Y1: a kernel client that opens the device is an event driver (IOHIDInterface opens us on its
 * driver's behalf: "open by AppleUserHIDEventDriver" in desktop-20261021-v2); a user client (an
 * IOHIDManager reader) is not, and takes nothing from our services. */
bool SnapI2CHIDDevice::handleOpen(IOService *client, IOOptionBits options, void *argument)
{
    bool ok = IOHIDDevice::handleOpen(client, options, argument);
    if (ok && client && !OSDynamicCast(IOUserClient, client))
        fBus->noteEventDriver(client, client->getMetaClass()->getClassName(), true);
    return ok;
}

void SnapI2CHIDDevice::handleClose(IOService *client, IOOptionBits options)
{
    IOHIDDevice::handleClose(client, options);
    if (client && !OSDynamicCast(IOUserClient, client))
        fBus->noteEventDriver(client, client->getMetaClass()->getClassName(), false);
}

IOReturn SnapI2CHIDDevice::setReport(IOMemoryDescriptor *report, IOHIDReportType type,
                                     IOOptionBits options)
{
    uint8_t buf[I2C_HID_MAX_OUTPUT + 1];
    uint8_t id = (uint8_t)(options & 0xff);
    IOByteCount len = report ? report->getLength() : 0;
    uint32_t kind;
    IOReturn r;

    if (type == kIOHIDReportTypeOutput)
        kind = SnapI2CHIDBus::kRequestOutput;
    else if (type == kIOHIDReportTypeFeature)
        kind = SnapI2CHIDBus::kRequestSetReport;
    else {
        logRequest("setReport", type, options, len, kIOReturnUnsupported);
        return kIOReturnUnsupported;
    }
    if (!len || len > I2C_HID_MAX_OUTPUT) {
        logRequest("setReport", type, options, len, kIOReturnBadArgument);
        return kIOReturnBadArgument;
    }
    report->readBytes(0, buf, len);
    r = fBus->request(kind, type == kIOHIDReportTypeOutput ? I2C_HID_TYPE_OUTPUT
                                                           : I2C_HID_TYPE_FEATURE,
                      id, buf, (uint32_t)len, nullptr);
    if (r == kIOReturnBadArgument && id && buf[0] != id) {
        /* The buffer may omit the numbered report's ID byte. */
        bcopy(buf, buf + 1, len);
        buf[0] = id;
        r = fBus->request(kind, type == kIOHIDReportTypeOutput ? I2C_HID_TYPE_OUTPUT
                                                               : I2C_HID_TYPE_FEATURE,
                          id, buf, (uint32_t)len + 1u, nullptr);
    }
    logRequest("setReport", type, options, len, r);
    return r;
}

IOReturn SnapI2CHIDDevice::getReport(IOMemoryDescriptor *report, IOHIDReportType type,
                                     IOOptionBits options)
{
    uint8_t buf[I2C_HID_MAX_INPUT];
    uint32_t got = 0;
    uint8_t i2c_type = type == kIOHIDReportTypeInput ? I2C_HID_TYPE_INPUT
                     : type == kIOHIDReportTypeFeature ? I2C_HID_TYPE_FEATURE : 0;
    IOReturn r;

    if (!report || !i2c_type) {
        logRequest("getReport", type, options, report ? report->getLength() : 0, kIOReturnUnsupported);
        return kIOReturnUnsupported;
    }
    r = fBus->request(SnapI2CHIDBus::kRequestGetReport, i2c_type, (uint8_t)(options & 0xff),
                      buf, sizeof(buf), &got);
    if (r == kIOReturnSuccess && got > report->getLength())
        r = kIOReturnOverrun;
    if (r == kIOReturnSuccess)
        report->writeBytes(0, buf, got);
    logRequest("getReport", type, options, got, r);
    return r;
}
