/* SPDX-License-Identifier: MIT */
/* SnapI2CHIDBus: publishing the HID services, gated HID requests, stop and teardown. Part of SnapI2CHID.cpp's
 * translation unit, included there in order; not compiled on its own. */

void SnapI2CHIDBus::publishCall(thread_call_param_t owner, thread_call_param_t)
{
    SnapI2CHIDBus *self = static_cast<SnapI2CHIDBus *>(owner);
    self->publish();
    __atomic_store_n(&self->fPublishing, 0u, __ATOMIC_RELEASE);
    self->release();                        /* step()'s reference */
}

void SnapI2CHIDBus::publish()
{
    SnapI2CHIDKeyboard *kbd;
    SnapI2CHIDDevice *hid;
    SnapI2CHIDMultitouch *mt = nullptr;
    bool ptp;

    if (fStopping)
        return;
    /* The event services first: they do not depend on the IOHIDDevice. The keyboard service
     * only for the A16 keyboard's layout (input report 54 of 9 bytes); the pointer service
     * whenever the report descriptor has a relative mouse collection or the device took the
     * Precision Touchpad switch (which runs here, before polling starts). */
    ptp = enablePrecisionTouchpad();
    {
        struct hid_pointer_layout layout;
        int lr = hid_pointer_layout(fDev->rdesc, fDev->rdesc_len, &layout);
        SnapI2CHIDPointer *ptr = lr == HID_POINTER_OK || ptp ? OSTypeAlloc(SnapI2CHIDPointer) : nullptr;
        if (lr != HID_POINTER_OK && !ptp) {
            IOLog(LOG_PREFIX "addr 0x%02x: no pointer service (%s)\n", fCfg.address,
                  lr == HID_POINTER_NO_MOUSE ? "no relative mouse collection" : "malformed descriptor");
        } else if (ptr && ptr->initWithBus(this, lr == HID_POINTER_OK ? &layout : nullptr, ptp ? &fPtp : nullptr) &&
                   ptr->attach(this) && ptr->start(this)) {
            if (!(ptr->getState() & kIOServiceRegisteredState))
                ptr->registerService();
            if (lr == HID_POINTER_OK)
                IOLog(LOG_PREFIX "pointer event service started: %s, primary 1/2, report %u (%u bytes), %u buttons, "
                      "x %u y %u wheel %u pan %u bits, %s mode, state 0x%x\n", ptr->getMetaClass()->getClassName(),
                      layout.report_id, layout.report_bytes, layout.buttons, layout.x.size, layout.y.size,
                      layout.wheel.size, layout.pan.size, ptp ? "touchpad" : "mouse", (unsigned)ptr->getState());
            else
                IOLog(LOG_PREFIX "pointer event service started: %s, primary 1/2, touchpad mode only, state 0x%x\n",
                      ptr->getMetaClass()->getClassName(), (unsigned)ptr->getState());
            fPointerStarted = ptr;
        } else {
            IOLog(LOG_PREFIX "pointer event service start failed\n");
            if (ptr) {
                ptr->detach(this);
                OSSafeReleaseNULL(ptr);
            }
        }
    }
    if (hid_rdesc_report_bytes(&fDev->rinfo, HID_REPORT_INPUT, HID_KBD_REPORT_KEYS) != HID_KBD_KEYS_BYTES)
        kbd = nullptr;
    else
        kbd = OSTypeAlloc(SnapI2CHIDKeyboard);
    if (!kbd) {
        /* not this device's layout */
    } else if (kbd->initWithBus(this) && kbd->attach(this)) {
        if (kbd->start(this)) {
            if (!(kbd->getState() & kIOServiceRegisteredState))
                kbd->registerService();
            IOLog(LOG_PREFIX "keyboard event service started: %s, primary %u/%u, transport I2C, state 0x%x\n",
                  kbd->getMetaClass()->getClassName(), 1u, 6u, (unsigned)kbd->getState());
            fKeyboardStarted = kbd;              /* keeps the allocation's reference */
        } else {
            IOLog(LOG_PREFIX "keyboard event service start failed\n");
            kbd->detach(this);
            OSSafeReleaseNULL(kbd);
        }
    } else {
        IOLog(LOG_PREFIX "cannot create the keyboard event service\n");
        OSSafeReleaseNULL(kbd);
    }
    /* Y2: a touchpad in Precision Touchpad mode gets no IOHIDDevice. Its reports are Touch Pad
     * digitizer frames: a generic HID event driver (DriverKit's AppleUserHIDEventService, which
     * matches any 13/x interface once its server runs) makes no pointer motion from them, and our
     * pointer would yield to it (installed-fresh-desktop-20261021-v2: dead touchpad). Without the
     * device nothing can match; the pointer service stays the touchpad's only event service. */
    if (ptp) {
        hid = nullptr;
        IOLog(LOG_PREFIX "addr 0x%02x: raw HID device not published: the precision touchpad is served by the "
              "pointer event service only\n", fCfg.address);
        mt = publishMultitouch();
    } else {
        hid = OSTypeAlloc(SnapI2CHIDDevice);
    }
    if (hid && (!hid->initWithBus(this) || !hid->attach(this))) {
        IOLog(LOG_PREFIX "cannot create the HID device\n");
        OSSafeReleaseNULL(hid);
    } else if (hid && !hid->start(this)) {
        IOLog(LOG_PREFIX "IOHIDDevice start failed\n");
        hid->detach(this);
        OSSafeReleaseNULL(hid);
    } else if (hid && !(hid->getState() & kIOServiceRegisteredState)) {
        /* IOHIDDevice::start publishes the device; make sure matching runs. */
        hid->registerService();
    }
    fGate->runAction(gatedRun, hid, mt);
}

IOReturn SnapI2CHIDBus::gatedRun(OSObject *owner, void *hid, void *mt, void *, void *)
{
    SnapI2CHIDBus *self = static_cast<SnapI2CHIDBus *>(owner);
    self->fHID = static_cast<SnapI2CHIDDevice *>(hid);   /* keeps publishCall's reference */
    self->fMT = static_cast<SnapI2CHIDMultitouch *>(mt); /* likewise */
    self->fRawPublished = hid != nullptr;
    self->fKeyboard = self->fKeyboardStarted;
    self->fPointer = self->fPointerStarted;
    if (i2c_hid_run(self->fDev) == I2C_HID_R_OK && !self->fStopping) {
        self->noteState();
        self->fTimer->setTimeoutUS(self->fCfg.poll_us);
    }
    return kIOReturnSuccess;
}

IOReturn SnapI2CHIDBus::gatedRequest(OSObject *owner, void *arg, void *, void *, void *)
{
    SnapI2CHIDBus *self = static_cast<SnapI2CHIDBus *>(owner);
    SnapRequest *req = static_cast<SnapRequest *>(arg);
    switch (req->kind) {
    case kRequestOutput:
        req->result = i2c_hid_send_output(self->fDev, req->buf, req->len);
        break;
    case kRequestSetReport:
        req->result = i2c_hid_set_report(self->fDev, req->type, req->buf, req->len);
        break;
    case kRequestGetReport:
        req->result = i2c_hid_get_report(self->fDev, req->type, req->id, req->buf, req->len,
                                         req->got);
        break;
    default:
        req->result = I2C_HID_R_INVALID;
        break;
    }
    self->noteState();
    return kIOReturnSuccess;
}

IOReturn SnapI2CHIDBus::request(uint32_t kind, uint8_t type, uint8_t id, uint8_t *buf,
                                uint32_t len, uint32_t *got)
{
    SnapRequest req = {kind, type, id, buf, len, got, I2C_HID_R_INVALID};
    if (fStopping || !fGate)
        return kIOReturnOffline;
    /* Re-entrant from deliver() on the work loop thread: the gate is recursive
     * and the bus is idle between transfers. */
    fGate->runAction(gatedRequest, &req);
    switch (req.result) {
    case I2C_HID_R_OK: return kIOReturnSuccess;
    case I2C_HID_R_EMPTY: return kIOReturnNotReady;
    case I2C_HID_R_NOT_READY: return kIOReturnNotReady;
    case I2C_HID_R_INVALID: return kIOReturnBadArgument;
    case I2C_HID_R_BAD_RESPONSE: return kIOReturnIOError;
    default: return kIOReturnIOError;
    }
}

IOReturn SnapI2CHIDBus::gatedStop(OSObject *owner, void *, void *, void *, void *)
{
    SnapI2CHIDBus *self = static_cast<SnapI2CHIDBus *>(owner);
    if (self->fKeyboard)
        self->fKeyboard->releaseKeys();
    if (self->fPointer)
        self->fPointer->releaseButtons();
    self->fKeyboard = nullptr;
    self->fPointer = nullptr;
    self->fMTNextCheck = ~0u;               /* M20: no registry walks or kicks while stopping */
    i2c_hid_stop(self->fDev);
    if (self->fBus.started && !self->fBus.wedged)
        (void)geni_i2c_stop(&self->fBus);        /* restore lengths and IRQ enables */
    return kIOReturnSuccess;
}

void SnapI2CHIDBus::stop(IOService *provider)
{
    fStopping = true;
    if (fTimer)
        fTimer->cancelTimeout();
    if (fPublish && thread_call_cancel(fPublish)) {
        /* Dequeued before it ran: drop the reference step() gave it. */
        __atomic_store_n(&fPublishing, 0u, __ATOMIC_RELEASE);
        release();
    }
    for (uint32_t ms = 0; __atomic_load_n(&fPublishing, __ATOMIC_ACQUIRE); ms++) {
        if (ms == 1000)
            IOLog(LOG_PREFIX "stop is waiting for the HID device's start to return\n");
        IOSleep(1);
    }
    if (fGate && fDev)
        fGate->runAction(gatedStop);
    releaseAll();
    IOService::stop(provider);
}

void SnapI2CHIDBus::releaseAll()
{
    if (fWorkLoop && fTimer)
        fWorkLoop->removeEventSource(fTimer);
    if (fWorkLoop && fGate)
        fWorkLoop->removeEventSource(fGate);
    OSSafeReleaseNULL(fTimer);
    OSSafeReleaseNULL(fGate);
    OSSafeReleaseNULL(fWorkLoop);
    if (fPublish) {
        thread_call_free(fPublish);
        fPublish = nullptr;
    }
    OSSafeReleaseNULL(fHID);
    OSSafeReleaseNULL(fMT);
    fKeyboard = nullptr;
    OSSafeReleaseNULL(fKeyboardStarted);
    fPointer = nullptr;
    OSSafeReleaseNULL(fPointerStarted);
    OSSafeReleaseNULL(fRegs);
    OSSafeReleaseNULL(fGpio);
    fBase = nullptr;
    fAttention = nullptr;
    if (fDev) {
        IOFree(fDev, sizeof(*fDev));
        fDev = nullptr;
    }
}

void SnapI2CHIDBus::free()
{
    releaseAll();
    IOService::free();
}
