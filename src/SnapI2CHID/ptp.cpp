/* SPDX-License-Identifier: MIT */
/* SnapI2CHIDBus and the Precision Touchpad: the feature-report hooks, the gated mode switch and its re-application,
 * and the multitouch device's publication and stack checks. Part of SnapI2CHID.cpp's translation unit, included
 * there in order; not compiled on its own. */

int SnapI2CHIDBus::ptpGet(void *ctx, uint8_t id, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    SnapI2CHIDBus *self = static_cast<SnapI2CHIDBus *>(ctx);
    return SNAP_I2C_HID_FEATURE_GET(self->fDev, id, buf, cap, got) != I2C_HID_R_OK;
}

int SnapI2CHIDBus::ptpSet(void *ctx, const uint8_t *report, uint32_t len)
{
    SnapI2CHIDBus *self = static_cast<SnapI2CHIDBus *>(ctx);
    return SNAP_I2C_HID_FEATURE_SET(self->fDev, report, len) != I2C_HID_R_OK;
}

/* The mode switch of hid-ptp.c (blob, capabilities, Input Mode 3, Surface/Button Switch,
 * readback) as one gated sequence: no poll runs between its transfers. */
IOReturn SnapI2CHIDBus::gatedPtp(OSObject *owner, void *setup, void *, void *, void *)
{
    SnapI2CHIDBus *self = static_cast<SnapI2CHIDBus *>(owner);
    const struct hid_ptp_io io = {self, ptpGet, ptpSet};
    hid_ptp_enable(&self->fPtp, &io, self->fPtpScratch, sizeof(self->fPtpScratch),
                   static_cast<struct hid_ptp_setup *>(setup));
    self->noteState();
    return kIOReturnSuccess;
}

/* At publish (READY, before polling): whether the device is a Precision Touchpad that took the
 * switch. Every outcome is logged; anything but a written mode leaves today's mouse mode (F1, F2,
 * F14, F16). The keyboard's descriptor has no Touch Pad collection: nothing is sent (F15). */
bool SnapI2CHIDBus::enablePrecisionTouchpad()
{
    struct hid_ptp_setup setup;
    char contacts[8], pad[8], blob[24], before[8];
    int lr = hid_ptp_layout(fDev->rdesc, fDev->rdesc_len, &fPtp);
    if (lr == HID_PTP_NO_TOUCHPAD)
        return false;
    if (lr != HID_PTP_OK) {
        IOLog(LOG_PREFIX "addr 0x%02x: precision touchpad not enabled (%s): mouse mode\n", fCfg.address,
              hid_ptp_layout_name(lr));
        return false;
    }
    fGate->runAction(gatedPtp, &setup);
    fPtpReinits = fDev->stats.reinits;
    fPtpArmed = setup.result == HID_PTP_ENABLED || setup.result == HID_PTP_ENABLED_UNVERIFIED;
    if (!fPtpArmed) {
        if (setup.result == HID_PTP_REFUSED_READBACK)
            IOLog(LOG_PREFIX "addr 0x%02x: precision touchpad not enabled (refused: readback %u): mouse mode\n",
                  fCfg.address, setup.mode_after);
        else
            IOLog(LOG_PREFIX "addr 0x%02x: precision touchpad not enabled (%s): mouse mode\n", fCfg.address,
                  hid_ptp_enable_name(setup.result));
        return false;
    }
    snprintf(contacts, sizeof(contacts), setup.caps_read ? "%u" : "?", setup.max_contacts);
    snprintf(pad, sizeof(pad), setup.caps_read ? "%u" : "?", setup.pad_type);
    snprintf(blob, sizeof(blob), setup.blob_read ? "%u bytes" : "not read", setup.blob_bytes);
    snprintf(before, sizeof(before), setup.mode_before_read ? "%u" : "?", setup.mode_before);
    IOLog(LOG_PREFIX "precision touchpad %s: report %u (%u bytes), fingers %u, confidence %s, contact count %s, "
          "scan time %s, buttons %u, %u.%u x %u.%u mm; contacts max %s, pad type %s; blob %s; input mode %s -> 3; "
          "surface and button switch %s\n", hid_ptp_enable_name(setup.result), fPtp.report_id, fPtp.report_bytes,
          fPtp.fingers, fPtp.has_confidence ? "yes" : "no", fPtp.contact_count.size ? "yes" : "no",
          fPtp.scan_time.size ? "yes" : "no", fPtp.buttons, fPtp.width_um / 1000u, fPtp.width_um % 1000u / 100u,
          fPtp.height_um / 1000u, fPtp.height_um % 1000u / 100u, contacts, pad, blob, before,
          setup.selective_written ? "on" : "not written");
    return true;
}

/* F10: a re-initialisation RESETs the device, which returns it to mouse mode; F24: so does a reset
 * the device did by itself, seen as report 1 while armed (the pointer asks, bounded). Once
 * RUNNING, apply the switch again and end whatever touch was in progress. Work loop (step). */
void SnapI2CHIDBus::notePtpReinit()
{
    struct hid_ptp_setup setup;
    bool reinit;
    if (!fPtpArmed || fDev->state != I2C_HID_S_RUNNING)
        return;
    reinit = fDev->stats.reinits != fPtpReinits;
    if (!reinit && !fPtpReapply)
        return;
    fPtpReinits = fDev->stats.reinits;
    fPtpReapply = false;
    gatedPtp(this, &setup, nullptr, nullptr, nullptr);       /* already on the work loop */
    if (reinit)
        IOLog(LOG_PREFIX "addr 0x%02x: precision touchpad re-applied after re-initialisation %u: %s\n",
              fCfg.address, fPtpReinits, hid_ptp_enable_name(setup.result));
    else
        IOLog(LOG_PREFIX "addr 0x%02x: precision touchpad re-applied after mouse-mode reports: %s\n", fCfg.address,
              hid_ptp_enable_name(setup.result));
    if (fPointer)
        fPointer->resetTouch();
    if (fMT)
        fMT->reset();                       /* M15: open touches end on the next ticks */
}

/* Multitouch emulation for an armed touchpad: on unless the node says snapintosh,multitouch = 0. */
bool SnapI2CHIDBus::multitouchWanted() const
{
    uint32_t on = 1;
    (void)readU32("snapintosh,multitouch", &on);
    return on != 0;
}

/* At publish, after the switch held (publish's thread): the Apple multitouch device, or nothing
 * (M12: a pad hid-mt2.c cannot describe; the node turned it off; IOHIDDevice start failed). Every
 * outcome is logged; without it the pointer service is the touchpad's only event service. */
SnapI2CHIDMultitouch *SnapI2CHIDBus::publishMultitouch()
{
    struct hid_mt2 geometry;
    SnapI2CHIDMultitouch *mt;
    int r;
    if (!multitouchWanted()) {
        IOLog(LOG_PREFIX "addr 0x%02x: multitouch device off (snapintosh,multitouch = 0)\n", fCfg.address);
        return nullptr;
    }
    r = hid_mt2_init(&geometry, &fPtp);
    if (r != HID_MT2_OK) {
        IOLog(LOG_PREFIX "addr 0x%02x: multitouch device not published (%s)\n", fCfg.address, hid_mt2_init_name(r));
        return nullptr;
    }
    mt = OSTypeAlloc(SnapI2CHIDMultitouch);
    if (!mt || !mt->initWithBus(this, &geometry) || !mt->attach(this)) {
        IOLog(LOG_PREFIX "addr 0x%02x: cannot create the multitouch device\n", fCfg.address);
        OSSafeReleaseNULL(mt);
        return nullptr;
    }
    if (!mt->start(this)) {
        IOLog(LOG_PREFIX "addr 0x%02x: multitouch device start failed\n", fCfg.address);
        mt->detach(this);
        OSSafeReleaseNULL(mt);
        return nullptr;
    }
    if (!(mt->getState() & kIOServiceRegisteredState))
        mt->registerService();
    IOLog(LOG_PREFIX "addr 0x%02x: multitouch device published: %04x:%04x, family 0x%02x, surface %u.%02u x %u.%02u mm, "
          "frames as report %u (0x%02x)\n", fCfg.address, HID_MT2_VENDOR, HID_MT2_PRODUCT, HID_MT2_FAMILY,
          (unsigned)(4 * geometry.wq) / 100u, (unsigned)(4 * geometry.wq) % 100u, (unsigned)(4 * geometry.hq) / 100u,
          (unsigned)(4 * geometry.hq) % 100u, HID_MT2_REPORT_ID, HID_MT2_FRAME_ID);
    return mt;
}

/* M1-M7, work loop (noteProgress, RUNNING): what sits above the multitouch device, every 32 polls
 * (about 0.25 s) until a frames consumer is there and for the first minute, then every 125. The
 * pointer hands the touchpad over exactly while a consumer is attached; an empty frame (at most 10,
 * one a second) makes Apple's driver build its multitouch device before the first touch. */
void SnapI2CHIDBus::noteMultitouch()
{
    const struct i2c_hid_stats *s = &fDev->stats;
    uint32_t bits;
    if (!fMT || s->polls < fMTNextCheck)
        return;
    bits = fMT->stack();
    fMTNextCheck = s->polls + ((bits & SnapI2CHIDMultitouch::kStackConsumer) && s->polls > 7500u ? 125u : 32u);
    if (bits != fMTStack) {
        IOLog(LOG_PREFIX "addr 0x%02x: multitouch stack: interface %s, %s, multitouch device %s, frames consumer %s "
              "(poll %u)\n", fCfg.address, bits & SnapI2CHIDMultitouch::kStackInterface ? "yes" : "no",
              bits & SnapI2CHIDMultitouch::kStackAppleDriver ? "Apple multitouch driver"
              : bits & SnapI2CHIDMultitouch::kStackOtherDriver ? "another driver (no handover)" : "no driver",
              bits & SnapI2CHIDMultitouch::kStackDevice ? "yes" : "no",
              bits & SnapI2CHIDMultitouch::kStackConsumer ? "yes" : "no", s->polls);
        fMTStack = bits;
    }
    if (fPointer)
        fPointer->handover((bits & SnapI2CHIDMultitouch::kStackConsumer) != 0);
    if ((bits & SnapI2CHIDMultitouch::kStackAppleDriver) && !(bits & SnapI2CHIDMultitouch::kStackDevice) &&
        fMTKicks < 10u && (!fMTKicks || s->polls - fMTLastKick >= 125u)) {
        fMT->kick();
        fMTKicks++;
        fMTLastKick = s->polls;
        IOLog(LOG_PREFIX "addr 0x%02x: multitouch: empty frame %u, for the driver to build its device\n", fCfg.address,
              fMTKicks);
    }
}
