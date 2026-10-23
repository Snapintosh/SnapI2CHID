/* SPDX-License-Identifier: MIT */
/* The Precision Touchpad mode switch (hid_ptp_enable). Part of hid-ptp.c's translation unit, included there in
 * order; not compiled on its own. */

/* ---------------------------------------------------------------------------- the switch ---- */

static int read_feature(const struct hid_ptp_io *io, const struct hid_ptp_feature *f, uint8_t *buf,
                        uint32_t cap, struct hid_ptp_setup *s)
{
    uint32_t got = 0;
    if (!f->report_id || f->report_bytes > cap)
        return 0;
    s->requests++;
    return !io->get_feature(io->ctx, f->report_id, buf, cap, &got) && got == f->report_bytes &&
           buf[0] == f->report_id;
}

int hid_ptp_enable(const struct hid_ptp_layout *l, const struct hid_ptp_io *io, uint8_t *scratch,
                   uint32_t cap, struct hid_ptp_setup *s)
{
    const struct hid_ptp_feature *m;
    uint32_t n;
    if (!s)
        return HID_PTP_ENABLE_INVALID;
    memset(s, 0, sizeof(*s));
    s->result = HID_PTP_ENABLE_INVALID;
    if (!l || !io || !io->get_feature || !io->set_feature || !scratch || !l->input_mode.report_id ||
        l->input_mode.report_bytes < 2u || l->input_mode.report_bytes > cap)
        return s->result;
    m = &l->input_mode;
    n = m->report_bytes;
    /* 1. The certification blob, once: some devices switch only after it was read. */
    if (read_feature(io, &l->blob, scratch, cap, s)) {
        s->blob_read = 1;
        s->blob_bytes = l->blob.report_bytes;
    }
    /* 2. Capabilities. */
    {
        const struct hid_ptp_feature *c = l->contact_max.report_id ? &l->contact_max : &l->pad_type;
        if (read_feature(io, c, scratch, cap, s)) {
            s->caps_read = 1;
            if (l->contact_max.report_id == c->report_id)
                s->max_contacts = (uint8_t)hid_ptp_get(scratch + 1, c->report_bytes - 1u, &l->contact_max.field);
            if (l->pad_type.report_id == c->report_id)
                s->pad_type = (uint8_t)hid_ptp_get(scratch + 1, c->report_bytes - 1u, &l->pad_type.field);
        }
    }
    /* 3. Input Mode = touchpad, on the report as read (other fields kept) or on zeros. */
    if (read_feature(io, m, scratch, cap, s)) {
        s->mode_before = (uint8_t)hid_ptp_get(scratch + 1, n - 1u, &m->field);
        s->mode_before_read = 1;
    } else {
        memset(scratch, 0, n);
        scratch[0] = m->report_id;
    }
    hid_ptp_put(scratch + 1, n - 1u, &m->field, HID_PTP_MODE_TOUCHPAD);
    s->requests++;
    if (io->set_feature(io->ctx, scratch, n))
        return s->result = HID_PTP_REFUSED_WRITE;
    /* 4. Surface and Button Switch on (their own report, or two). */
    {
        const struct hid_ptp_feature *sw[2] = {&l->surface_switch, &l->button_switch};
        for (uint32_t i = 0; i < 2; i++) {
            uint32_t bytes = sw[i]->report_bytes;
            if (!sw[i]->report_id || bytes < 2u || bytes > cap || (i && sw[1]->report_id == sw[0]->report_id))
                continue;
            memset(scratch, 0, bytes);
            scratch[0] = sw[i]->report_id;
            for (uint32_t j = 0; j < 2; j++)
                if (sw[j]->report_id == sw[i]->report_id)
                    hid_ptp_put(scratch + 1, bytes - 1u, &sw[j]->field, 1u);
            s->requests++;
            if (!io->set_feature(io->ctx, scratch, bytes))
                s->selective_written = 1;
        }
    }
    /* 5. Read Input Mode back. */
    if (read_feature(io, m, scratch, cap, s)) {
        s->mode_after = (uint8_t)hid_ptp_get(scratch + 1, n - 1u, &m->field);
        s->mode_after_read = 1;
        return s->result = s->mode_after == HID_PTP_MODE_TOUCHPAD ? HID_PTP_ENABLED : HID_PTP_REFUSED_READBACK;
    }
    return s->result = HID_PTP_ENABLED_UNVERIFIED;
}

const char *hid_ptp_enable_name(int r)
{
    switch (r) {
    case HID_PTP_ENABLED: return "enabled";
    case HID_PTP_ENABLED_UNVERIFIED: return "enabled, unverified";
    case HID_PTP_REFUSED_WRITE: return "refused: write";
    case HID_PTP_REFUSED_READBACK: return "refused: readback";
    case HID_PTP_ENABLE_INVALID: return "no input mode";
    default: return "unknown";
    }
}
