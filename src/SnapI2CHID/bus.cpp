/* SPDX-License-Identifier: MIT */
/* SnapI2CHIDBus: the MMIO and transport hooks, start, the poll loop and its SNAP_I2C_HID_LATENCY scheduling, the
 * state and progress logs, and yielding to an event driver. Part of SnapI2CHID.cpp's translation unit, included
 * there in order; not compiled on its own. */

/* --------------------------------------------------------------- bus ---- */

struct SnapRequest {
    uint32_t kind;
    uint8_t type, id;
    uint8_t *buf;
    uint32_t len, *got;
    int result;
};

/* The only MMIO in this file. tools/test-snap-i2c-hid-start.cpp defines these
 * two to its controller model; the kext uses the plain volatile accesses. */
#ifndef SNAP_I2C_HID_MMIO_READ
#define SNAP_I2C_HID_MMIO_READ(base, offset) ((base)[(offset) / 4])
#define SNAP_I2C_HID_MMIO_WRITE(base, offset, value) ((base)[(offset) / 4] = (value))
#endif

uint32_t SnapI2CHIDBus::ioRead32(void *ctx, uint32_t offset)
{
    SnapI2CHIDBus *self = static_cast<SnapI2CHIDBus *>(ctx);
    return SNAP_I2C_HID_MMIO_READ(self->fBase, offset);
}

void SnapI2CHIDBus::ioWrite32(void *ctx, uint32_t offset, uint32_t value)
{
    SnapI2CHIDBus *self = static_cast<SnapI2CHIDBus *>(ctx);
    SNAP_I2C_HID_MMIO_WRITE(self->fBase, offset, value);
}

uint64_t SnapI2CHIDBus::ioNowUs(void *)
{
    uint64_t abs, ns;
    clock_get_uptime(&abs);
    absolutetime_to_nanoseconds(abs, &ns);
    return ns / 1000u;
}

void SnapI2CHIDBus::ioDelayUs(void *, uint32_t us)
{
#if SNAP_I2C_HID_LATENCY
    /* T4: whole milliseconds asleep and the remainder the way the status polls already wait (IODelay below 1 ms):
     * a 52-byte input read waits 1 ms + 193 us, not 2 ms. */
    if (us >= 1000u)
        IOSleep(us / 1000u);
    if (us % 1000u)
        IODelay(us % 1000u);
#else
    /* Transfers wait their nominal wire time (about 1.5 ms for a 64-byte
     * input read): sleep for whole milliseconds, spin only below that. */
    if (us >= 1000u)
        IOSleep((us + 999u) / 1000u);
    else
        IODelay(us);
#endif
}

int SnapI2CHIDBus::attention(void *ctx)
{
    SnapI2CHIDBus *self = static_cast<SnapI2CHIDBus *>(ctx);
    uint32_t level = *self->fAttention & 1u;     /* TLMM GPIO_IN_OUT bit 0: input */
    return self->fAttentionActiveLow ? level == 0u : level == 1u;
}

void SnapI2CHIDBus::deliver(void *ctx, const uint8_t *report, uint32_t len)
{
    SnapI2CHIDBus *self = static_cast<SnapI2CHIDBus *>(ctx);
    if (self->fKeyboard)
        self->fKeyboard->handleReport(report, len);
    if (self->fPointer)
        self->fPointer->handleReport(report, len);
    if (self->fHID)
        self->fHID->deliverReport(report, len);
}

bool SnapI2CHIDBus::readU32(const char *key, uint32_t *out, uint32_t count) const
{
    OSData *data = OSDynamicCast(OSData, fProvider->getProperty(key));
    if (!data || data->getLength() != count * sizeof(uint32_t))
        return false;
    const uint8_t *p = static_cast<const uint8_t *>(data->getBytesNoCopy());
    for (uint32_t i = 0; i < count; i++)
        out[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 |
                 (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    return true;
}

OSString *SnapI2CHIDBus::copyProduct() const
{
    OSData *data = OSDynamicCast(OSData, fProvider->getProperty("snapintosh,hid-product"));
    if (data && data->getLength() > 1 && data->getLength() < 64) {
        char name[64];
        bcopy(data->getBytesNoCopy(), name, data->getLength());
        name[data->getLength() - 1] = 0;
        return OSString::withCString(name);
    }
    return OSString::withCString("I2C HID Device");
}

bool SnapI2CHIDBus::start(IOService *provider)
{
    uint32_t address = 0, descReg = 0, vidpid[2] = {0, 0}, att[2], poll = 8000;

    if (!IOService::start(provider))
        return false;
    fProvider = provider;
    if (!readU32("snapintosh,hid-address", &address) || address > 0x7f ||
        !readU32("snapintosh,hid-descriptor-reg", &descReg) || descReg > 0xffff) {
        IOLog(LOG_PREFIX "node lacks hid-address/hid-descriptor-register\n");
        goto fail;
    }
    (void)readU32("snapintosh,hid-vendor-product", vidpid, 2);
    (void)readU32("snapintosh,bus-hz", &fBusHz);
    (void)readU32("snapintosh,poll-us", &poll);
    (void)readU32("snapintosh,irq-policy", &fPolicy);

    /* reg[0] is the one 4 KiB page the monitor maps (compat/i2c-input.h); a
     * missing, short or unaligned window means the loader and monitor did not
     * provide it, and nothing here touches the bus. */
    fRegs = provider->mapDeviceMemoryWithIndex(0);
    if (!fRegs || fRegs->getLength() < GENI_WINDOW_BYTES ||
        (fRegs->getPhysicalAddress() & (GENI_WINDOW_BYTES - 1))) {
        IOLog(LOG_PREFIX "cannot map the serial-engine page\n");
        goto fail;
    }
    fBase = reinterpret_cast<volatile uint32_t *>(fRegs->getVirtualAddress());
    fLocation = (uint32_t)(fRegs->getPhysicalAddress() & 0xffffff00u) | address;
    if (readU32("snapintosh,attention", att, 2)) {
        fGpio = provider->mapDeviceMemoryWithIndex(1);
        if (fGpio && !(att[0] & 3u) && att[0] + 4u <= fGpio->getLength()) {
            fAttention = reinterpret_cast<volatile uint32_t *>(fGpio->getVirtualAddress() + att[0]);
            fAttentionActiveLow = att[1] != 0;
        } else {
            IOLog(LOG_PREFIX "attention property without a usable reg[1]; polling the bus\n");
        }
    }

    fDev = static_cast<struct i2c_hid_dev *>(IOMallocZero(sizeof(*fDev)));
    fWorkLoop = IOWorkLoop::workLoop();
    fGate = IOCommandGate::commandGate(this);
    fTimer = IOTimerEventSource::timerEventSource(this, timerAction);
    fPublish = thread_call_allocate(publishCall, this);
    if (!fDev || !fWorkLoop || !fGate || !fTimer || !fPublish ||
        fWorkLoop->addEventSource(fGate) != kIOReturnSuccess ||
        fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess)
        goto fail;

    i2c_hid_default_config(&fCfg, (uint8_t)address, (uint16_t)descReg);
    fCfg.expect_vendor = (uint16_t)vidpid[0];
    fCfg.expect_product = (uint16_t)vidpid[1];
    if (poll >= 1000u && poll <= 100000u)
        fCfg.poll_us = poll;
#if SNAP_I2C_HID_LATENCY
    {   /* T1: snap_i2c_active_us=0 keeps the configured period throughout; anything else at least 2 ms */
        uint32_t active = SNAP_I2C_HID_ACTIVE_POLL_US;
        (void)PE_parse_boot_argn("snap_i2c_active_us", &active, sizeof(active));
        if (active && active < SNAP_I2C_HID_MIN_POLL_US)
            active = SNAP_I2C_HID_MIN_POLL_US;
        fActiveUs = active;
    }
#endif
    fStartUs = ioNowUs(this);
    if (fGate->runAction(gatedStart) != kIOReturnSuccess)
        goto fail;
    registerService();
    fTimer->setTimeoutUS(1);
    return true;
fail:
    releaseAll();
    IOService::stop(provider);
    return false;
}

IOReturn SnapI2CHIDBus::gatedStart(OSObject *owner, void *, void *, void *, void *)
{
    SnapI2CHIDBus *self = static_cast<SnapI2CHIDBus *>(owner);
    const struct geni_i2c_io io = {self, ioRead32, ioWrite32, ioNowUs, ioDelayUs};
    int r = geni_i2c_start(&self->fBus, &io, self->fBusHz, self->fPolicy);
    const struct geni_i2c *b = &self->fBus;
    if (r != GENI_I2C_OK) {
        /* The registers as found, before any write (geni-i2c.h); no further MMIO. */
        IOLog(LOG_PREFIX "controller refused (%s): status 0x%x irq 0x%x irq-en 0x%x tx 0x%x rx 0x%x, "
              "recovered %u words\n", geni_i2c_result_name(r), b->found_status, b->found_irq,
              b->found_irq_en, b->found_tx_fifo, b->found_rx_fifo, b->stats.recovered_words);
        return kIOReturnNotReady;
    }
    if (b->recovered)
        IOLog(LOG_PREFIX "recovered a previous run's controller state: status 0x%x irq 0x%x irq-en 0x%x "
              "tx 0x%x rx 0x%x; discarded %u words, %u cancels, %u aborts\n", b->found_status,
              b->found_irq, b->found_irq_en, b->found_tx_fifo, b->found_rx_fifo,
              b->stats.recovered_words, b->stats.cancels, b->stats.aborts);
    if (i2c_hid_begin(self->fDev, &self->fBus, &self->fCfg, deliver, self) != I2C_HID_R_OK) {
        (void)geni_i2c_stop(&self->fBus);
        return kIOReturnBadArgument;
    }
    if (self->fAttention)
        i2c_hid_set_attention(self->fDev, attention, self);
    IOLog(LOG_PREFIX "addr 0x%02x clock 0x%x scl 0x%x fifo %u/%u policy %u%s\n",
          self->fCfg.address, self->fBus.clock_cfg, self->fBus.scl_counters,
          self->fBus.tx_fifo_words, self->fBus.rx_fifo_words, self->fPolicy,
          self->fAttention ? " attention" : "");
    return kIOReturnSuccess;
}

void SnapI2CHIDBus::timerAction(OSObject *owner, IOTimerEventSource *)
{
    static_cast<SnapI2CHIDBus *>(owner)->step();
}

void SnapI2CHIDBus::step()
{
    uint32_t wake;
    if (fStopping || !fDev)
        return;
#if SNAP_I2C_HID_LATENCY
    const uint64_t startUs = ioNowUs(this);
    const uint32_t reports = fDev->stats.reports;
    noteWake(startUs);
#endif
    wake = i2c_hid_step(fDev);
    noteState();
    noteProgress();
    notePtpReinit();
    yieldToEventDriver();
    if (fPointer)
        fPointer->tick();                   /* tap release, the lift window, momentum */
    if (fMT)
        fMT->tick();                        /* lifted contacts' last stages, the empty frame (M8) */
    if (fDev->state == I2C_HID_S_READY && !fHID) {
        /* IOHIDDevice::start may call back into this driver; run it outside
         * the work loop, then resume polling through gatedRun. The callout
         * holds a reference until it returns. */
        if (!__atomic_load_n(&fPublishing, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&fPublishing, 1u, __ATOMIC_RELEASE);
            retain();
            if (thread_call_enter(fPublish))
                release();                  /* already queued: keep one reference */
        }
        return;
    }
    if (wake != I2C_HID_NO_WAKE)
#if SNAP_I2C_HID_LATENCY
        armNextStep(wake, startUs, reports);
#else
        fTimer->setTimeoutUS(wake ? wake : 1);
#endif
}

#if SNAP_I2C_HID_LATENCY
/* T3: how late the timer fired against the time asked for, as a decaying estimate (each sample capped at 2 ms). */
void SnapI2CHIDBus::noteWake(uint64_t startUs)
{
    if (!fArmedForUs)
        return;
    const uint64_t late = startUs > fArmedForUs ? startUs - fArmedForUs : 0;
    fSlopUs = (fSlopUs * 7u + (late < 2000u ? late : 2000u)) / 8u;
    fArmedForUs = 0;
}

/* T1-T3: in RUNNING, the next poll is due one period after this one's deadline (or its start, after a late one). The
 * period is the active one while a report came in the last SNAP_I2C_HID_ACTIVE_HOLD_US, the configured one otherwise.
 * The poll comes at least a quarter period (1 ms minimum) after this step's end. The timer is asked early by the
 * measured lateness, at most min(1 ms, period / 8). Any other state or wake keeps today's relative delay. */
void SnapI2CHIDBus::armNextStep(uint32_t wake, uint64_t startUs, uint32_t reports)
{
    if (fDev->state != I2C_HID_S_RUNNING || wake != fCfg.poll_us || !fActiveUs) {
        fDeadlineUs = 0;
        fTimer->setTimeoutUS(wake ? wake : 1);
        return;
    }
    const uint64_t now = ioNowUs(this);
    if (fDev->stats.reports != reports)
        fLastReportUs = now ? now : 1;
    uint32_t period = fCfg.poll_us;
    if (fLastReportUs && now - fLastReportUs < SNAP_I2C_HID_ACTIVE_HOLD_US && fActiveUs < period) {
        period = fActiveUs;
        fActivePolls++;
    } else {
        fIdlePolls++;
    }
    uint64_t base = fDeadlineUs ? fDeadlineUs : startUs;
    if (base + period <= startUs)
        base = startUs;                                    /* far behind: re-anchor, no burst of catch-up polls */
    uint64_t next = base + period;
    const uint64_t gap = period / 4u > 1000u ? period / 4u : 1000u;
    if (next < now + gap)
        next = now + gap;                                  /* T2 */
    fDeadlineUs = next;
    uint64_t lead = fSlopUs;
    const uint64_t cap = period / 8u < 1000u ? period / 8u : 1000u;
    if (lead > cap)
        lead = cap;                                        /* T3 */
    uint64_t delay = next - now;
    delay = delay > lead ? delay - lead : 1;
    fArmedForUs = now + delay;
    fTimer->setTimeoutUS((uint32_t)delay);
}
#endif

/* One line per state change (about eight in a healthy start), so a stopped
 * guest's console shows how far initialisation got without anyone at the
 * keyboard: the reset acknowledgment's length prefix (00 00 expected), then
 * READY and RUNNING. */
void SnapI2CHIDBus::noteState()
{
    uint32_t previous = fLastState;
    if (fDev->state == fLastState)
        return;
    fLastState = fDev->state;
    IOLog(LOG_PREFIX "addr 0x%02x %s -> %s at %llu ms\n", fCfg.address,
          previous == ~0u ? "start" : i2c_hid_state_name(previous), i2c_hid_state_name(fDev->state),
          (ioNowUs(this) - fStartUs) / 1000u);
    if (previous == I2C_HID_S_RESET_WAIT && fDev->state != I2C_HID_S_FAILED)
        IOLog(LOG_PREFIX "addr 0x%02x reset acknowledged: length prefix %02x %02x after %llu ms, "
              "%u NACK retries, %u early reports, %u bad lengths\n", fCfg.address,
              fDev->inbuf[0], fDev->inbuf[1], (ioNowUs(this) - fDev->reset_at_us) / 1000u,
              fDev->stats.reset_retries, fDev->stats.early_reports, fDev->stats.bad_length);
    if (fDev->state == I2C_HID_S_FAILED)
        IOLog(LOG_PREFIX "addr 0x%02x FAILED in %s: %s (bus %s, status 0x%x tx 0x%x rx 0x%x)\n",
              fCfg.address, i2c_hid_state_name(fDev->failed_in),
              i2c_hid_failure_name(fDev->failure), geni_i2c_result_name(fDev->last_result),
              fBus.last_status, fBus.last_tx_fifo, fBus.last_rx_fifo);
    else if (fDev->state == I2C_HID_S_READY || fDev->state == I2C_HID_S_RUNNING)
        IOLog(LOG_PREFIX "addr 0x%02x %s: %04x:%04x report descriptor %u bytes, input %u "
              "(%u declared input reports longer)\n",
              fCfg.address, i2c_hid_state_name(fDev->state), fDev->desc.vendor_id,
              fDev->desc.product_id, fDev->rdesc_len, fDev->desc.max_input, fDev->stats.oversize_inputs);
}

/* Progress without a person: polling counters at about 1 s, 10 s and 60 s of
 * RUNNING and then every 10 minutes, and the first few input reports by ID and
 * size only, never their contents. */
void SnapI2CHIDBus::noteProgress()
{
    const struct i2c_hid_stats *s = &fDev->stats;
    if (fDev->state != I2C_HID_S_RUNNING)
        return;
    if (!fRunningAtUs)
        fRunningAtUs = ioNowUs(this);
    noteMultitouch();
    if (fHID && fRegistryLogs < 2) {
        uint64_t at = fRegistryLogs ? 30000000u : 2000000u;
        if (ioNowUs(this) - fRunningAtUs >= at) {
            fHID->logRegistry(fRegistryLogs ? "30 s" : "2 s");
            fRegistryLogs++;
        }
    }
    if (s->reports > fLoggedReports && fLoggedReports < 4) {
        fLoggedReports = s->reports;
        IOLog(LOG_PREFIX "addr 0x%02x input report %u: id %u, %u bytes\n", fCfg.address, s->reports,
              fDev->last_report_len ? fDev->last_report[0] : 0u, fDev->last_report_len);
    }
    if (s->polls < fNextStatsPoll)
        return;
    IOLog(LOG_PREFIX "addr 0x%02x polls %u: empty %u reports %u bogus %u identical %u rejected %u "
          "errors %u nacks %u reinits %u; bus ok %u timeouts %u cancels %u aborts %u\n",
          fCfg.address, s->polls, s->empty, s->reports, s->bogus, s->identical,
          s->bad_length + s->unknown_id + s->wrong_size, s->transfer_errors, s->nacks, s->reinits,
          fBus.stats.ok, fBus.stats.timeouts, fBus.stats.cancels, fBus.stats.aborts);
#if SNAP_I2C_HID_LATENCY
    IOLog(LOG_PREFIX "addr 0x%02x latency: active %u us, %u active and %u idle polls, timer slop %llu us\n",
          fCfg.address, fActiveUs, fActivePolls, fIdlePolls, fSlopUs);
#endif
    noteServices();
    fNextStatsPoll = fNextStatsPoll < 1250u ? 1250u : fNextStatsPoll < 7500u ? 7500u
                   : fNextStatsPoll + 75000u;
}

/* D1: with every polls line (so a console tail has it), who turns this device's reports into
 * events: our services (dispatching or yielded), whether the device's own IOHIDDevice exists, and
 * which event driver holds it open. */
void SnapI2CHIDBus::noteServices()
{
    IOLog(LOG_PREFIX "addr 0x%02x services: keyboard %s, pointer %s%s; raw HID device %s; event drivers open %u%s%s; "
          "multitouch device %s, stack 0x%x\n",
          fCfg.address, fKeyboard ? fKeyboard->state() : "none", fPointer ? fPointer->state() : "none",
          fPointer ? (fPtpArmed ? " (precision touchpad)" : " (mouse mode)") : "",
          fRawPublished ? "published" : "not published", driverOpens(),
          fDriverName[0] ? " last " : "", fDriverName, fMT ? "published" : "none", fMT ? fMTStack : 0u);
}

/* Y1-Y5: an event driver that opened our IOHIDDevice (DriverKit's AppleUserHIDEventService once
 * its server runs, or any kernel HID event driver) dispatches the device's reports itself: our own
 * services yield while it holds the device open and resume when it closes it (its dext exited),
 * so keys are never doubled and never lost. An armed touchpad has no IOHIDDevice (Y2): nothing
 * can take it from our pointer, which no generic driver can replace. Called from handleOpen and
 * handleClose (any thread); yieldToEventDriver applies it on the work loop (step). */
void SnapI2CHIDBus::noteEventDriver(const void *client, const char *cls, bool open)
{
    uintptr_t key = reinterpret_cast<uintptr_t>(client);
    for (uint32_t i = 0; i < kDriverSlots; i++) {
        uintptr_t expected = key;
        if (__atomic_load_n(&fDriverClients[i], __ATOMIC_ACQUIRE) != key)
            continue;
        if (!open)
            __atomic_compare_exchange_n(&fDriverClients[i], &expected, (uintptr_t)0, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE);
        return;                                          /* already held (open twice) or now released */
    }
    if (!open)
        return;
    for (uint32_t i = 0; i < kDriverSlots; i++) {
        uintptr_t expected = 0;
        if (__atomic_compare_exchange_n(&fDriverClients[i], &expected, key, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            break;
    }
    unsigned n = 0;
    for (; cls && cls[n] && n + 1 < sizeof(fDriverName); n++)
        fDriverName[n] = cls[n];
    fDriverName[n] = 0;
}

uint32_t SnapI2CHIDBus::driverOpens() const
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < kDriverSlots; i++)
        n += __atomic_load_n(&fDriverClients[i], __ATOMIC_ACQUIRE) != 0;
    return n;
}

void SnapI2CHIDBus::yieldToEventDriver()
{
    uint32_t opens = driverOpens();
    if ((opens != 0) == (fDriverOpensApplied != 0)) {
        fDriverOpensApplied = opens;
        return;
    }
    fDriverOpensApplied = opens;
    if (opens) {
        if (fKeyboard)
            fKeyboard->suppress(fDriverName);
        if (fPointer)
            fPointer->suppress(fDriverName);
    } else {
        IOLog(LOG_PREFIX "addr 0x%02x: %s closed the HID device; our services dispatch again\n", fCfg.address,
              fDriverName[0] ? fDriverName : "the event driver");
        if (fKeyboard)
            fKeyboard->resume();
        if (fPointer)
            fPointer->resume();
    }
    noteServices();
}
