/* SPDX-License-Identifier: MIT */
/* The Precision Touchpad report decoder and gesture engine (pointer, taps, two-finger scroll, momentum). Part of
 * hid-ptp.c's translation unit, included there in order; not compiled on its own. */

/* ------------------------------------------------------------------------------ engine ---- */

int hid_ptp_decode(const struct hid_ptp_layout *l, const uint8_t *report, uint32_t len,
                   struct hid_ptp_frame *f)
{
    const uint8_t *body;
    uint32_t bytes;
    if (!l || !report || !len || !f)
        return HID_PTP_BAD_LENGTH;
    if (report[0] != l->report_id)
        return HID_PTP_OTHER_REPORT;
    if (len != l->report_bytes)
        return HID_PTP_BAD_LENGTH;
    body = report + 1;
    bytes = len - 1u;
    memset(f, 0, sizeof(*f));
    f->slots = l->fingers;
    for (uint32_t i = 0; i < l->fingers; i++) {
        const struct hid_ptp_finger *fi = &l->finger[i];
        struct hid_ptp_contact *c = &f->c[i];
        c->tip = hid_ptp_get(body, bytes, &fi->tip) != 0;
        c->confident = l->has_confidence ? hid_ptp_get(body, bytes, &fi->confidence) != 0 : 1;
        c->id = fi->id.size ? hid_ptp_get(body, bytes, &fi->id) : i;
        c->x = sget(body, bytes, &fi->x);
        c->y = sget(body, bytes, &fi->y);
    }
    f->count = hid_ptp_get(body, bytes, &l->contact_count);
    f->scan = hid_ptp_get(body, bytes, &l->scan_time);
    for (uint32_t b = 0; b < l->buttons && b < HID_PTP_MAX_BUTTONS; b++)
        if (l->button[b].size && hid_ptp_get(body, bytes, &l->button[b]))
            f->buttons |= 1u << b;
    return HID_PTP_FRAME;
}

void hid_ptp_engine_init(struct hid_ptp_engine *e, const struct hid_ptp_layout *l)
{
    uint64_t xr, yr;
    memset(e, 0, sizeof(*e));
    e->l = *l;
    xr = l->x_max > l->x_min ? (uint64_t)((int64_t)l->x_max - l->x_min) : 1u;
    yr = l->y_max > l->y_min ? (uint64_t)((int64_t)l->y_max - l->y_min) : 1u;
    e->q16_x = ((uint64_t)l->width_um * HID_PTP_UNITS_PER_INCH << 32) / (25400u * xr);
    e->q16_y = ((uint64_t)l->height_um * HID_PTP_UNITS_PER_INCH << 32) / (25400u * yr);
    e->um_x = ((uint64_t)l->width_um << 16) / xr;
    e->um_y = ((uint64_t)l->height_um << 16) / yr;
}

static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

/* Counts to output units (Q16 of 1/400 inch). */
static int32_t qx(const struct hid_ptp_engine *e, int64_t counts) { return (int32_t)((counts * (int64_t)e->q16_x) / 65536); }
static int32_t qy(const struct hid_ptp_engine *e, int64_t counts) { return (int32_t)((counts * (int64_t)e->q16_y) / 65536); }

/* Distance in micrometres (the larger axis). */
static uint32_t um(const struct hid_ptp_engine *e, int64_t dx, int64_t dy)
{
    uint64_t ux = ((uint64_t)iabs64(dx) * e->um_x) >> 16, uy = ((uint64_t)iabs64(dy) * e->um_y) >> 16;
    uint64_t m = ux > uy ? ux : uy;
    return m > 0xffffffffu ? 0xffffffffu : (uint32_t)m;
}

/* Output units (Q16) to micrometres. */
static uint32_t units_um(int64_t q16)
{
    return (uint32_t)(((uint64_t)iabs64(q16) * 25400u / HID_PTP_UNITS_PER_INCH) >> 16);
}

static void emit_buttons(struct hid_ptp_engine *e, const struct hid_ptp_sink *s)
{
    uint32_t b = e->mapped | e->tap;
    if (b == e->last_buttons)
        return;
    e->last_buttons = b;
    s->pointer(s->ctx, 0, 0, b);
}

static void release_tap(struct hid_ptp_engine *e, const struct hid_ptp_sink *s)
{
    e->tap = 0;
    emit_buttons(e, s);
}

static void end_momentum(struct hid_ptp_engine *e, const struct hid_ptp_sink *s)
{
    if (e->momentum && e->momentum_started)
        s->scroll(s->ctx, 0, 0, HID_PTP_SCROLL_MOMENTUM_END);
    e->momentum = e->momentum_started = 0;
}

/* Ended; then momentum when allowed and the release was quick (motion near `ref`). */
static void end_scroll(struct hid_ptp_engine *e, uint64_t now, uint64_t ref, int momentum,
                       const struct hid_ptp_sink *s)
{
    int64_t speed = iabs64(e->vel_v) > iabs64(e->vel_h) ? iabs64(e->vel_v) : iabs64(e->vel_h);
    s->scroll(s->ctx, 0, 0, HID_PTP_SCROLL_PHASE_ENDED);
    if (momentum && e->have_vel && ref >= e->last_scroll_us && ref - e->last_scroll_us <= HID_PTP_MOMENTUM_RECENT_US &&
        speed >= ((int64_t)HID_PTP_MOMENTUM_START << 16)) {
        e->momentum = 1;
        e->momentum_started = 0;
        e->mom_v = e->vel_v;
        e->mom_h = e->vel_h;
        e->mom_at = e->mom_start = now;
        e->stats.momentum++;
    }
}

static struct hid_ptp_track *find(struct hid_ptp_engine *e, uint32_t id)
{
    for (uint32_t i = 0; i < HID_PTP_MAX_CONTACTS; i++)
        if (e->t[i].used && e->t[i].id == id)
            return &e->t[i];
    return NULL;
}

static void note_ended(struct hid_ptp_engine *e, uint32_t id)
{
    e->ended[e->nended % 8u] = id;
    e->nended++;
}

static int ended_before(const struct hid_ptp_engine *e, uint32_t id)
{
    for (uint32_t i = 0; i < e->nended && i < 8u; i++)
        if (e->ended[i] == id)
            return 1;
    return 0;
}

/* Frame time from Scan Time (100 us units, wrapping at its field size), else the host clock. */
static uint64_t frame_dt(struct hid_ptp_engine *e, uint32_t scan, uint64_t now)
{
    uint64_t host = e->last_us && now > e->last_us ? now - e->last_us : 0, dt;
    if (host && host < 1000u)
        host = 1000u;
    if (host > 100000u)
        host = 100000u;
    if (!e->l.scan_time.size)
        return host;
    if (!e->have_scan) {
        dt = host;
    } else {
        uint32_t mask = e->l.scan_time.size >= 32u ? 0xffffffffu : (1u << e->l.scan_time.size) - 1u;
        uint32_t d = (scan - e->last_scan) & mask;
        if (!d) {
            dt = 0;                                          /* the same frame again */
        } else if (d >= 10u && d <= 1000u) {
            dt = (uint64_t)d * 100u;
        } else {
            e->stats.scan_fallbacks++;
            dt = host;
        }
    }
    e->last_scan = scan;
    e->have_scan = 1;
    return dt;
}

static void pointer_frame(struct hid_ptp_engine *e, uint64_t now, int pick_most_moved, const struct hid_ptp_sink *s)
{
    struct hid_ptp_track *best = NULL;
    int64_t best_m = -1;
    for (uint32_t i = 0; i < HID_PTP_MAX_CONTACTS; i++) {
        struct hid_ptp_track *t = &e->t[i];
        int64_t m;
        if (!t->used || t->palm || !t->moved)
            continue;
        m = iabs64(t->dx) + iabs64(t->dy);
        if (!best || (pick_most_moved && m > best_m)) {
            best = t;
            best_m = m;
        }
    }
    if (!best || (!best->dx && !best->dy) || now < e->settle_until)
        return;
    e->last_buttons = e->mapped | e->tap;
    e->stats.pointer_events++;
    s->pointer(s->ctx, qx(e, best->dx), qy(e, best->dy), e->last_buttons);
}

/* Two fingers: the mean motion of the pair, both present in the last frame too. */
static int pair_motion(struct hid_ptp_engine *e, int64_t *v, int64_t *h, struct hid_ptp_track **pair)
{
    uint32_t n = 0;
    int64_t sx = 0, sy = 0;
    for (uint32_t i = 0; i < HID_PTP_MAX_CONTACTS && n < 2; i++) {
        struct hid_ptp_track *t = &e->t[i];
        if (!t->used || t->palm)
            continue;
        pair[n++] = t;
        sx += t->dx;
        sy += t->dy;
    }
    if (n != 2 || !pair[0]->moved || !pair[1]->moved)
        return 0;
    *v = -(int64_t)qy(e, sy) / 2;                            /* fingers up: wheel up (positive) */
    *h = -(int64_t)qx(e, sx) / 2;
    return 1;
}

static void two_finger_frame(struct hid_ptp_engine *e, uint64_t now, uint64_t dt, const struct hid_ptp_sink *s)
{
    struct hid_ptp_track *pair[2];
    int64_t v, h;
    if (!pair_motion(e, &v, &h, pair))
        return;
    if (dt) {
        int64_t sv = v * 1000000 / (int64_t)dt, sh = h * 1000000 / (int64_t)dt;
        if (!e->have_vel) {
            e->vel_v = sv;
            e->vel_h = sh;
            e->have_vel = 1;
        } else {
            e->vel_v = (e->vel_v + sv) / 2;
            e->vel_h = (e->vel_h + sh) / 2;
        }
    }
    if (e->mode == HID_PTP_TWO) {
        uint32_t m0 = um(e, pair[0]->x - pair[0]->xe, pair[0]->y - pair[0]->ye);
        uint32_t m1 = um(e, pair[1]->x - pair[1]->xe, pair[1]->y - pair[1]->ye);
        uint32_t still = m0 < m1 ? m0 : m1, moving = m0 < m1 ? m1 : m0;
        e->acc_v += v;
        e->acc_h += h;
        if (moving >= HID_PTP_REST_MOVE_UM && still <= HID_PTP_REST_STILL_UM) {
            e->mode = HID_PTP_REST;                          /* F23: the still one is a resting thumb */
            return;
        }
        if ((units_um(e->acc_v) >= HID_PTP_SCROLL_START_UM || units_um(e->acc_h) >= HID_PTP_SCROLL_START_UM) &&
            still >= HID_PTP_SCROLL_EACH_UM && (v || h)) {
            e->mode = HID_PTP_SCROLL;
            e->s_scrolled = 1;
            e->stats.scrolls++;
            e->stats.scroll_events++;
            e->last_scroll_us = now;
            s->scroll(s->ctx, (int32_t)v, (int32_t)h, HID_PTP_SCROLL_PHASE_BEGAN);
        }
        return;
    }
    if (v || h) {                                            /* SCROLL */
        e->last_scroll_us = now;
        e->stats.scroll_events++;
        s->scroll(s->ctx, (int32_t)v, (int32_t)h, HID_PTP_SCROLL_PHASE_CHANGED);
    }
}

static void enter_two(struct hid_ptp_engine *e)
{
    e->mode = HID_PTP_TWO;
    e->acc_v = e->acc_h = 0;
    e->vel_v = e->vel_h = 0;
    e->have_vel = 0;
    for (uint32_t i = 0; i < HID_PTP_MAX_CONTACTS; i++) {
        e->t[i].xe = e->t[i].x;
        e->t[i].ye = e->t[i].y;
    }
}

static void end_session(struct hid_ptp_engine *e, uint64_t now, const struct hid_ptp_sink *s)
{
    if (e->session && !e->s_press && !e->s_scrolled && !e->s_palm && !e->s_far && e->s_max >= 1 &&
        e->s_max <= 2 && now - e->s_start <= HID_PTP_TAP_MAX_US) {
        if (e->s_typing) {
            e->stats.typing_blocked_taps++;
        } else {
            e->tap = e->s_max == 2 ? 2u : 1u;
            e->tap_release_at = now + HID_PTP_TAP_RELEASE_US;
            e->stats.taps++;
            if (e->s_max == 2)
                e->stats.secondary_taps++;
            emit_buttons(e, s);
        }
    }
    e->session = 0;
    e->nended = 0;
    e->have_scan = 0;
    e->settle_until = 0;                                     /* a settle never outlives its touch */
    e->mode = HID_PTP_IDLE;
}

static void process(struct hid_ptp_engine *e, const struct hid_ptp_contact *c, uint32_t n, uint32_t scan,
                    uint32_t phys, uint64_t now, uint64_t last_key, const struct hid_ptp_sink *s)
{
    uint32_t fingers = 0, prev = e->fingers, any = 0, used = 0;
    uint64_t dt;
    e->stats.frames++;
    if (s->frame)
        s->frame(s->ctx, c, n, phys, scan, e->l.scan_time.size != 0);
    for (uint32_t i = 0; i < n; i++)
        any |= c[i].tip;
    if (e->tap && (any || now >= e->tap_release_at))
        release_tap(e, s);
    if (e->momentum && any)
        end_momentum(e, s);
    dt = frame_dt(e, scan, now);
    for (uint32_t i = 0; i < HID_PTP_MAX_CONTACTS; i++) {
        e->t[i].seen = 0;
        e->t[i].moved = 0;
        e->t[i].dx = e->t[i].dy = 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        struct hid_ptp_track *t = find(e, c[i].id);
        if (t && t->seen) {
            e->stats.duplicate_ids++;
            continue;
        }
        if (!c[i].tip) {
            if (t)
                t->seen = 2;                                 /* lifting: ends below */
            continue;
        }
        e->stats.contacts++;
        if (t) {
            int32_t dx = c[i].x - t->x, dy = c[i].y - t->y;
            t->seen = 1;
            if (um(e, dx, dy) > HID_PTP_JUMP_UM) {           /* F6: a new contact under an old ID */
                e->stats.jumps++;
                e->s_far = 1;
                t->x = t->x0 = t->xe = c[i].x;
                t->y = t->y0 = t->ye = c[i].y;
            } else {
                uint32_t far;
                t->dx = dx;
                t->dy = dy;
                t->moved = 1;
                t->x = c[i].x;
                t->y = c[i].y;
                far = um(e, t->x - t->x0, t->y - t->y0);
                if (far > t->travel_um)
                    t->travel_um = far;
                if (t->travel_um > HID_PTP_TAP_TRAVEL_UM)
                    e->s_far = 1;
            }
            if (!c[i].confident && !t->palm) {               /* F5: a palm for the rest of its life */
                t->palm = 1;
                e->stats.palms++;
                e->s_palm = 1;
            }
            continue;
        }
        for (uint32_t j = 0; j < HID_PTP_MAX_CONTACTS; j++) {
            if (e->t[j].used)
                continue;
            t = &e->t[j];
            memset(t, 0, sizeof(*t));
            t->used = 1;
            t->seen = 1;
            t->id = c[i].id;
            t->x = t->x0 = t->xe = c[i].x;
            t->y = t->y0 = t->ye = c[i].y;
            if (!c[i].confident) {
                t->palm = 1;
                e->stats.palms++;
                e->s_palm = 1;
            }
            if (!e->session) {
                e->session = 1;
                e->s_start = now;
                e->s_max = e->s_press = e->s_scrolled = e->s_far = 0;
                e->s_palm = t->palm;
                e->s_typing = last_key && (last_key >= now || now - last_key < HID_PTP_TYPING_US);
            } else if (ended_before(e, t->id)) {
                e->stats.reused_ids++;
            }
            break;
        }
    }
    for (uint32_t i = 0; i < HID_PTP_MAX_CONTACTS; i++) {
        struct hid_ptp_track *t = &e->t[i];
        if (!t->used)
            continue;
        if (t->seen != 1) {                                  /* lifted, or vanished from the frame */
            note_ended(e, t->id);
            t->used = 0;
            continue;
        }
        used++;
        if (!t->palm)
            fingers++;
    }
    if (fingers > e->stats.max_fingers)
        e->stats.max_fingers = fingers;
    if (fingers > e->s_max)
        e->s_max = (uint8_t)fingers;
    if (prev && fingers && fingers != prev)
        e->settle_until = now + HID_PTP_SETTLE_US;            /* F8: 1 <-> 2, not a touch's first or last frame */
    /* Physical buttons (F18, F19). */
    if (phys && !e->phys) {
        e->mapped = e->l.buttons >= 2u ? phys : fingers >= 2u ? 2u : 1u;
        e->s_press = 1;
        e->settle_until = now + HID_PTP_SETTLE_US;
        e->stats.button_edges++;
        if (e->mode == HID_PTP_SCROLL || e->mode == HID_PTP_SCROLL_LIFT) {
            end_scroll(e, now, now, 0, s);
            e->mode = HID_PTP_POINTER;
        }
        emit_buttons(e, s);
    } else if (!phys && e->phys) {
        e->mapped = 0;
        e->settle_until = now + HID_PTP_SETTLE_US;
        e->stats.button_edges++;
        emit_buttons(e, s);
    } else if (phys && e->l.buttons >= 2u && phys != e->mapped) {
        e->mapped = phys;
        emit_buttons(e, s);
    }
    e->phys = phys;
    /* What the fingers do. */
    if (e->mode == HID_PTP_SCROLL) {
        if (fingers == 2 && !phys) {
            two_finger_frame(e, now, prev == 2 ? dt : 0, s);
        } else if (fingers == 1 && !phys) {
            e->mode = HID_PTP_SCROLL_LIFT;                   /* F7: wait for the other lift */
            e->lift_at = now;
        } else if (!fingers) {
            end_scroll(e, now, now, 1, s);
            e->mode = HID_PTP_IDLE;
        } else {
            end_scroll(e, now, now, 0, s);
            e->mode = fingers >= 3 ? HID_PTP_MULTI : HID_PTP_POINTER;
        }
    } else if (e->mode == HID_PTP_SCROLL_LIFT) {
        if (!fingers) {
            end_scroll(e, now, e->lift_at, 1, s);
            e->mode = HID_PTP_IDLE;
        } else if (fingers == 2 && !phys) {
            e->mode = HID_PTP_SCROLL;                        /* the finger came back */
        } else if (fingers != 1 || phys || now - e->lift_at >= HID_PTP_LIFT_WINDOW_US) {
            end_scroll(e, now, now, 0, s);                   /* the pointer starts from the next frame */
            e->mode = fingers >= 3 ? HID_PTP_MULTI : HID_PTP_POINTER;
        }
    } else if (phys) {
        e->mode = HID_PTP_POINTER;
        pointer_frame(e, now, 1, s);                         /* a drag: the finger that moves */
    } else if (fingers == 1) {
        e->mode = HID_PTP_POINTER;
        pointer_frame(e, now, 0, s);
    } else if (fingers == 2) {
        if (prev != 2 || (e->mode != HID_PTP_TWO && e->mode != HID_PTP_REST))
            enter_two(e);
        if (e->mode == HID_PTP_REST)
            pointer_frame(e, now, 1, s);
        else
            two_finger_frame(e, now, prev == 2 ? dt : 0, s);
    } else {
        e->mode = fingers ? HID_PTP_MULTI : HID_PTP_IDLE;
    }
    e->fingers = fingers;
    e->last_us = now;
    if (!used)
        end_session(e, now, s);
}

int hid_ptp_engine_report(struct hid_ptp_engine *e, const uint8_t *report, uint32_t len, uint64_t now,
                          uint64_t last_key, const struct hid_ptp_sink *s)
{
    struct hid_ptp_frame f;
    int r = hid_ptp_decode(&e->l, report, len, &f);
    if (r == HID_PTP_OTHER_REPORT)
        return r;
    e->stats.reports++;
    if (r != HID_PTP_FRAME) {
        e->stats.bad_length++;
        return r;
    }
    if (!e->l.contact_count.size) {                          /* no count: the slots touching */
        struct hid_ptp_contact c[HID_PTP_MAX_FINGERS];
        uint32_t n = 0;
        for (uint32_t i = 0; i < f.slots; i++)
            if (f.c[i].tip)
                c[n++] = f.c[i];
        process(e, c, n, f.scan, f.buttons, now, last_key, s);
        return r;
    }
    if (f.count) {
        uint32_t want = f.count < HID_PTP_MAX_CONTACTS ? f.count : HID_PTP_MAX_CONTACTS;
        if (e->pending_want) {                               /* F13: a partial frame never completed */
            e->stats.hybrid_dropped++;
            e->pending_want = 0;
        }
        if (want > f.slots) {
            e->pending = f;
            e->pending.slots = f.slots;
            e->pending_want = want;
            e->stats.hybrid_partial++;
            return r;
        }
        process(e, f.c, want, f.scan, f.buttons, now, last_key, s);
        return r;
    }
    if (e->pending_want) {                                   /* continuation: Contact Count 0 */
        uint32_t need = e->pending_want - e->pending.slots;
        uint32_t take = need < f.slots ? need : f.slots;
        for (uint32_t i = 0; i < take; i++)
            e->pending.c[e->pending.slots + i] = f.c[i];
        e->pending.slots = (uint8_t)(e->pending.slots + take);
        if (e->pending.slots < e->pending_want)
            return r;
        e->pending_want = 0;
        process(e, e->pending.c, e->pending.slots, e->pending.scan, e->pending.buttons, now, last_key, s);
        return r;
    }
    process(e, f.c, 0, f.scan, f.buttons, now, last_key, s);
    return r;
}

void hid_ptp_engine_tick(struct hid_ptp_engine *e, uint64_t now, const struct hid_ptp_sink *s)
{
    if (e->tap && now >= e->tap_release_at)
        release_tap(e, s);
    if (e->mode == HID_PTP_SCROLL_LIFT && now - e->lift_at >= HID_PTP_LIFT_WINDOW_US) {
        end_scroll(e, now, now, 0, s);
        e->mode = HID_PTP_POINTER;
    }
    if (e->momentum && now > e->mom_at) {
        uint64_t dt = now - e->mom_at;
        int64_t speed, dv, dh;
        if (dt > 100000u)
            dt = 100000u;
        for (uint64_t ms = 0; ms < dt / 1000u; ms++) {
            e->mom_v = e->mom_v * HID_PTP_MOMENTUM_DECAY_Q16 / 65536;
            e->mom_h = e->mom_h * HID_PTP_MOMENTUM_DECAY_Q16 / 65536;
        }
        e->mom_at = now;
        speed = iabs64(e->mom_v) > iabs64(e->mom_h) ? iabs64(e->mom_v) : iabs64(e->mom_h);
        if (speed < ((int64_t)HID_PTP_MOMENTUM_STOP << 16) || now - e->mom_start >= HID_PTP_MOMENTUM_MAX_US) {
            end_momentum(e, s);
            return;
        }
        dv = e->mom_v * (int64_t)dt / 1000000;
        dh = e->mom_h * (int64_t)dt / 1000000;
        s->scroll(s->ctx, (int32_t)dv, (int32_t)dh,
                  e->momentum_started ? HID_PTP_SCROLL_MOMENTUM_CONTINUE : HID_PTP_SCROLL_MOMENTUM_START);
        e->momentum_started = 1;
        e->stats.momentum_events++;
    }
}

int hid_ptp_engine_busy(const struct hid_ptp_engine *e)
{
    return e->tap || e->momentum || e->mode == HID_PTP_SCROLL_LIFT;
}

void hid_ptp_engine_reset(struct hid_ptp_engine *e, uint64_t now, const struct hid_ptp_sink *s)
{
    (void)now;
    if (e->mode == HID_PTP_SCROLL || e->mode == HID_PTP_SCROLL_LIFT)
        s->scroll(s->ctx, 0, 0, HID_PTP_SCROLL_PHASE_ENDED);
    end_momentum(e, s);
    e->mapped = e->tap = e->phys = 0;
    emit_buttons(e, s);
    memset(e->t, 0, sizeof(e->t));
    e->mode = HID_PTP_IDLE;
    e->fingers = 0;
    e->session = 0;
    e->nended = 0;
    e->pending_want = 0;
    e->settle_until = 0;
    e->have_scan = 0;
    e->have_vel = 0;
    e->last_us = 0;
}
