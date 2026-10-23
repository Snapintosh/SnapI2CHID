/* SPDX-License-Identifier: MIT */
/* The Precision Touchpad report-descriptor parser (hid_ptp_layout) and its field accessors. Part of hid-ptp.c's
 * translation unit, included there in order; not compiled on its own. */

/* ------------------------------------------------------------------------------ layout ---- */

#define MAX_USAGES   32u
#define MAX_DEPTH    16u
#define MAX_STACK    8u
#define MAX_IDS      256u
#define MAX_INPUT_BITS   (512u * 8u)      /* I2C_HID_MAX_INPUT */
#define MAX_FEATURE_BITS (1024u * 8u)

#define U_TOUCHPAD   0x000d0005u
#define U_CONFIG     0x000d000eu
#define U_FINGER     0x000d0022u
#define U_TIP        0x000d0042u
#define U_CONFIDENCE 0x000d0047u
#define U_INPUT_MODE 0x000d0052u
#define U_CONTACT_ID 0x000d0051u
#define U_COUNT      0x000d0054u
#define U_COUNT_MAX  0x000d0055u
#define U_SCAN_TIME  0x000d0056u
#define U_SURFACE    0x000d0057u
#define U_BUTTON_SW  0x000d0058u
#define U_PAD_TYPE   0x000d0059u
#define U_X          0x00010030u
#define U_Y          0x00010031u
#define U_BLOB       0xff0000c5u

struct globals {
    uint32_t page, unit, size, count, id;
    int32_t lmin, lmax, pmin, pmax, uexp;
    uint32_t lmax_raw;
    uint8_t lmax_bytes;
};

struct axis_units { int32_t pmin, pmax, uexp; uint32_t unit; uint8_t have; };

/* About 1.1 KiB on the caller's stack (re-entrant: two buses publish on their own threads). */
struct parse {
    uint16_t in_bits[MAX_IDS];    /* bits so far per report ID (MAX_*_BITS fit) */
    uint16_t feat_bits[MAX_IDS];
    uint32_t fingers_seen;        /* finger collections, including any beyond the table */
    uint8_t finger_bad[HID_PTP_MAX_FINGERS];
    uint8_t input_id_set, touchpad_seen, oversize;
    struct axis_units ux, uy;     /* finger 0's X and Y */
};

static uint32_t uvalue(const uint8_t *p, uint32_t n)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int32_t svalue(const uint8_t *p, uint32_t n)
{
    uint32_t v = uvalue(p, n);
    if (n == 1)
        return (int8_t)v;
    if (n == 2)
        return (int16_t)v;
    return (int32_t)v;
}

static void set_field(struct hid_ptp_field *f, uint32_t offset, const struct globals *g)
{
    f->offset = (uint16_t)offset;
    f->size = (uint8_t)g->size;
    f->min = g->lmin;
    /* A logical maximum below the minimum was meant unsigned (a common descriptor slip). */
    f->max = g->lmax < g->lmin && g->lmax_bytes < 4 ? (int32_t)g->lmax_raw : g->lmax;
    f->is_signed = g->lmin < 0;
}

static void set_feature(struct hid_ptp_feature *f, uint32_t id, uint32_t offset, const struct globals *g)
{
    if (f->report_id)
        return;                   /* the first one counts (Linux: a second Input Mode is skipped) */
    f->report_id = (uint8_t)id;
    set_field(&f->field, offset, g);
}

static int extent_um(const struct axis_units *u, uint32_t *um)
{
    int64_t span = (int64_t)u->pmax - u->pmin;
    uint64_t v, base;
    int32_t e = u->uexp;
    if (!u->have || span <= 0 || (u->unit >> 8) || ((u->unit >> 4) & 0xfu) != 1u)
        return 0;
    if ((u->unit & 0xfu) == 1u)
        base = 10000u;            /* SI linear: centimetres */
    else if ((u->unit & 0xfu) == 3u)
        base = 25400u;            /* English linear: inches */
    else
        return 0;
    v = (uint64_t)span * base;
    for (; e > 0; e--) {
        v *= 10u;
        if (v > 100000000000ull)
            return 0;
    }
    for (; e < 0; e++)
        v /= 10u;
    if (v < 10000u || v > 1000000u)   /* 10 mm .. 1 m */
        return 0;
    *um = (uint32_t)v;
    return 1;
}

int hid_ptp_layout(const uint8_t *d, uint32_t len, struct hid_ptp_layout *out)
{
    struct parse pp, *p = &pp;
    struct globals g, stack[MAX_STACK];
    uint32_t usages[MAX_USAGES], nusages = 0, umin = 0, umax = 0, have_range = 0;
    uint32_t depth = 0, sp = 0;
    uint32_t touchpad_depth = 0, finger_depth = 0, finger = 0;
    struct hid_ptp_layout *l = out;          /* filled in place; only HID_PTP_OK makes it valid */
    int unsupported = 0;

    if (!d || !out)
        return HID_PTP_MALFORMED;
    memset(p, 0, sizeof(*p));
    memset(l, 0, sizeof(*l));
    memset(&g, 0, sizeof(g));
    for (uint32_t at = 0; at < len;) {
        uint8_t prefix = d[at];
        uint32_t size = prefix & 3u, type = (prefix >> 2) & 3u, tag = prefix >> 4, u;
        const uint8_t *data;
        if (prefix == 0xfe) {                                /* long item: skipped */
            if (len - at < 3u || len - at - 3u < d[at + 1])
                return HID_PTP_MALFORMED;
            at += 3u + d[at + 1];
            continue;
        }
        if (size == 3u)
            size = 4u;
        if (len - at < 1u + size)
            return HID_PTP_MALFORMED;
        data = d + at + 1;
        u = uvalue(data, size);
        at += 1u + size;
        if (type == 1u) {                                    /* global */
            switch (tag) {
            case 0x0: g.page = u; break;
            case 0x1: g.lmin = svalue(data, size); break;
            case 0x2: g.lmax = svalue(data, size); g.lmax_raw = u; g.lmax_bytes = (uint8_t)size; break;
            case 0x3: g.pmin = svalue(data, size); break;
            case 0x4: g.pmax = svalue(data, size); break;
            case 0x5:                                        /* 4-bit two's complement in one byte */
                g.uexp = size == 1u && u <= 0xfu ? (u > 7u ? (int32_t)u - 16 : (int32_t)u) : svalue(data, size);
                break;
            case 0x6: g.unit = u; break;
            case 0x7: g.size = u; break;
            case 0x8:
                if (size != 1u || !u)
                    return HID_PTP_MALFORMED;
                g.id = u;
                break;
            case 0x9: g.count = u; break;
            case 0xa:
                if (sp == MAX_STACK)
                    return HID_PTP_MALFORMED;
                stack[sp++] = g;
                break;
            case 0xb:
                if (!sp)
                    return HID_PTP_MALFORMED;
                g = stack[--sp];
                break;
            default: break;
            }
            continue;
        }
        if (type == 2u) {                                    /* local */
            uint32_t usage = size == 4u ? u : (g.page << 16) | u;
            if (tag == 0x0 && nusages < MAX_USAGES)
                usages[nusages++] = usage;
            else if (tag == 0x1) { umin = usage; have_range |= 1u; }
            else if (tag == 0x2) { umax = usage; have_range |= 2u; }
            continue;
        }
        if (type != 0u)
            return HID_PTP_MALFORMED;
        /* main */
        if (tag == 0xa) {                                    /* Collection */
            uint32_t first = nusages ? usages[0] : (have_range & 1u) ? umin : 0u;
            if (depth == MAX_DEPTH)
                return HID_PTP_MALFORMED;
            depth++;
            if (u == 1u && first == U_TOUCHPAD && !touchpad_depth && !p->touchpad_seen) {
                touchpad_depth = depth;
                p->touchpad_seen = 1;
            } else if (touchpad_depth && !finger_depth && first == U_FINGER && u != 1u) {
                finger_depth = depth;
                finger = p->fingers_seen++;
            }
        } else if (tag == 0xc) {                             /* End Collection */
            if (!depth)
                return HID_PTP_MALFORMED;
            if (finger_depth == depth)
                finger_depth = 0;
            if (touchpad_depth == depth)
                touchpad_depth = 0;
            depth--;
        } else if (tag == 0x8 || tag == 0xb || tag == 0x9) { /* Input, Feature, Output */
            uint32_t id = g.id, total = g.size * g.count;
            uint16_t *bits = tag == 0x8 ? p->in_bits : tag == 0xb ? p->feat_bits : NULL;
            uint32_t limit = tag == 0x8 ? MAX_INPUT_BITS : MAX_FEATURE_BITS;
            if (!depth || !g.size || g.size > 4096u || g.count > 4096u || id >= MAX_IDS)
                return HID_PTP_MALFORMED;
            if (bits && (uint32_t)bits[id] + total > limit)
                return HID_PTP_MALFORMED;
            if (bits && !(u & 1u) && (u & 2u)) {             /* data, variable */
                for (uint32_t i = 0; i < g.count; i++) {
                    uint32_t usage = i < nusages ? usages[i]
                                   : (have_range == 3u && umin + i <= umax) ? umin + i
                                   : nusages ? usages[nusages - 1] : 0u;
                    uint32_t off = bits[id] + i * g.size;
                    int wanted = 0;
                    if (tag == 0x8 && touchpad_depth) {
                        if (!p->input_id_set) {
                            l->report_id = (uint8_t)id;
                            p->input_id_set = 1;
                        }
                        if (id != l->report_id)
                            continue;                        /* not the multi-contact report */
                        if (finger_depth && finger < HID_PTP_MAX_FINGERS) {
                            struct hid_ptp_finger *f = &l->finger[finger];
                            struct hid_ptp_field *slot = usage == U_TIP ? &f->tip
                                                       : usage == U_CONFIDENCE ? &f->confidence
                                                       : usage == U_CONTACT_ID ? &f->id
                                                       : usage == U_X ? &f->x : usage == U_Y ? &f->y : NULL;
                            if (slot && !slot->size) {
                                wanted = 1;
                                set_field(slot, off, &g);
                                if ((usage == U_X || usage == U_Y) && (u & 4u))
                                    p->finger_bad[finger] = 1;   /* relative: not a touchpad contact */
                                if (finger == 0 && (usage == U_X || usage == U_Y)) {
                                    struct axis_units *a = usage == U_X ? &p->ux : &p->uy;
                                    a->pmin = g.pmin;
                                    a->pmax = g.pmax;
                                    a->uexp = g.uexp;
                                    a->unit = g.unit;
                                    a->have = 1;
                                }
                            }
                        } else if (!finger_depth) {
                            if (usage == U_COUNT && !l->contact_count.size) {
                                wanted = 1;
                                set_field(&l->contact_count, off, &g);
                            } else if (usage == U_SCAN_TIME && !l->scan_time.size) {
                                wanted = 1;
                                set_field(&l->scan_time, off, &g);
                            } else if ((usage >> 16) == 0x9u && (usage & 0xffffu) >= 1u &&
                                       (usage & 0xffffu) <= HID_PTP_MAX_BUTTONS) {
                                struct hid_ptp_field *b = &l->button[(usage & 0xffffu) - 1u];
                                if (!b->size) {
                                    wanted = 1;
                                    set_field(b, off, &g);
                                    if ((usage & 0xffffu) > l->buttons)
                                        l->buttons = (uint8_t)(usage & 0xffffu);
                                }
                            }
                        }
                    } else if (tag == 0xb) {
                        struct hid_ptp_feature *f = usage == U_INPUT_MODE ? &l->input_mode
                                                  : usage == U_SURFACE ? &l->surface_switch
                                                  : usage == U_BUTTON_SW ? &l->button_switch
                                                  : usage == U_COUNT_MAX ? &l->contact_max
                                                  : usage == U_PAD_TYPE ? &l->pad_type
                                                  : usage == U_BLOB && i == 0 ? &l->blob : NULL;
                        if (f && id) {
                            wanted = 1;
                            set_feature(f, id, off, &g);
                        }
                    }
                    if (wanted && g.size > 32u && usage != U_BLOB)
                        unsupported = 1;
                }
            }
            if (bits)
                bits[id] = (uint16_t)(bits[id] + total);
        }
        if (type == 0u) {                                    /* every main item clears the locals */
            nusages = 0;
            have_range = 0;
        }
    }
    if (depth || sp)
        return HID_PTP_MALFORMED;
    if (!p->touchpad_seen)
        return HID_PTP_NO_TOUCHPAD;
    if (p->fingers_seen > HID_PTP_MAX_FINGERS || unsupported || !l->report_id)
        return HID_PTP_UNSUPPORTED;
    l->fingers = (uint8_t)p->fingers_seen;
    if (!l->fingers)
        return HID_PTP_NO_FINGERS;
    l->has_confidence = 1;
    for (uint32_t i = 0; i < l->fingers; i++) {
        const struct hid_ptp_finger *f = &l->finger[i];
        if (p->finger_bad[i] || !f->tip.size || !f->x.size || !f->y.size || f->x.max <= f->x.min ||
            f->y.max <= f->y.min)
            return HID_PTP_NO_FINGERS;
        if (!f->confidence.size)
            l->has_confidence = 0;
    }
    if (!l->input_mode.report_id)
        return HID_PTP_NO_INPUT_MODE;
    if (!extent_um(&p->ux, &l->width_um) || !extent_um(&p->uy, &l->height_um))
        return HID_PTP_NO_UNITS;
    l->x_min = l->finger[0].x.min;
    l->x_max = l->finger[0].x.max;
    l->y_min = l->finger[0].y.min;
    l->y_max = l->finger[0].y.max;
    l->report_bytes = (uint16_t)((p->in_bits[l->report_id] + 7u) / 8u + 1u);
    {
        struct hid_ptp_feature *fs[6] = {&l->input_mode, &l->surface_switch, &l->button_switch,
                                         &l->contact_max, &l->pad_type, &l->blob};
        for (uint32_t i = 0; i < 6; i++)
            if (fs[i]->report_id)
                fs[i]->report_bytes = (uint16_t)((p->feat_bits[fs[i]->report_id] + 7u) / 8u + 1u);
    }
    if (l->input_mode.report_bytes < 2u)
        return HID_PTP_NO_INPUT_MODE;
    return HID_PTP_OK;
}

const char *hid_ptp_layout_name(int r)
{
    switch (r) {
    case HID_PTP_OK: return "ok";
    case HID_PTP_NO_TOUCHPAD: return "no touch pad collection";
    case HID_PTP_NO_FINGERS: return "no usable finger collection";
    case HID_PTP_NO_INPUT_MODE: return "no input mode feature";
    case HID_PTP_NO_UNITS: return "no physical units";
    case HID_PTP_UNSUPPORTED: return "unsupported layout";
    case HID_PTP_MALFORMED: return "malformed descriptor";
    default: return "unknown";
    }
}

uint32_t hid_ptp_get(const uint8_t *body, uint32_t bytes, const struct hid_ptp_field *f)
{
    uint32_t v = 0;
    for (uint32_t i = 0; f && i < f->size && i < 32u; i++) {
        uint32_t bit = f->offset + i;
        if (bit / 8u < bytes && (body[bit / 8u] >> (bit % 8u) & 1u))
            v |= 1u << i;
    }
    return v;
}

static int32_t sget(const uint8_t *body, uint32_t bytes, const struct hid_ptp_field *f)
{
    uint32_t v = hid_ptp_get(body, bytes, f);
    if (f->is_signed && f->size && f->size < 32u && (v >> (f->size - 1u)) & 1u)
        v |= ~0u << f->size;
    return (int32_t)v;
}

void hid_ptp_put(uint8_t *body, uint32_t bytes, const struct hid_ptp_field *f, uint32_t value)
{
    for (uint32_t i = 0; f && i < f->size && i < 32u; i++) {
        uint32_t bit = f->offset + i;
        if (bit / 8u >= bytes)
            break;
        if ((value >> i) & 1u)
            body[bit / 8u] = (uint8_t)(body[bit / 8u] | (1u << (bit % 8u)));
        else
            body[bit / 8u] = (uint8_t)(body[bit / 8u] & ~(1u << (bit % 8u)));
    }
}
