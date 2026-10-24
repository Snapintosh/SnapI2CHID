/* SPDX-License-Identifier: MIT */
/* Report-to-key translation; the contract and layouts are in hid-keyboard.h. */
#include "hid-keyboard.h"

#include <string.h>

static int held(const uint8_t keys[6], uint8_t usage)
{
    for (unsigned i = 0; i < 6; i++)
        if (keys[i] == usage)
            return 1;
    return 0;
}

/* A usable key usage: an actual key, inside the descriptor's logical range. */
static int key(uint8_t usage)
{
    return usage >= 0x04u && usage <= HID_KBD_MAX_KEY;
}

int hid_kbd_translate(struct hid_kbd_state *s, const uint8_t *r, uint32_t len,
                      hid_kbd_emit_fn emit, void *ctx)
{
    if (!s || !r || !len || !emit)
        return HID_KBD_BAD_LENGTH;
    if (r[0] == HID_KBD_REPORT_KEYS) {
        uint8_t mods, keys[6];
        if (len != HID_KBD_KEYS_BYTES)
            return HID_KBD_BAD_LENGTH;
        for (unsigned i = 0; i < 6; i++)
            if (r[3 + i] == HID_KBD_ROLLOVER)
                return HID_KBD_ROLLOVER_REPORT;      /* too many keys: keep the last good state */
        mods = r[1];
        for (unsigned i = 0; i < 6; i++) {
            keys[i] = key(r[3 + i]) ? r[3 + i] : 0u;
            for (unsigned j = 0; j < i; j++)
                if (keys[j] == keys[i])
                    keys[i] = 0u;                /* a usage listed twice is one key */
        }
        for (unsigned b = 0; b < 8; b++)
            if ((s->modifiers & (1u << b)) && !(mods & (1u << b)))
                emit(ctx, HID_KBD_PAGE_KEYBOARD, 0xe0u + b, 0);
        for (unsigned i = 0; i < 6; i++)
            if (s->keys[i] && !held(keys, s->keys[i]))
                emit(ctx, HID_KBD_PAGE_KEYBOARD, s->keys[i], 0);
        for (unsigned b = 0; b < 8; b++)
            if (!(s->modifiers & (1u << b)) && (mods & (1u << b)))
                emit(ctx, HID_KBD_PAGE_KEYBOARD, 0xe0u + b, 1);
        for (unsigned i = 0; i < 6; i++)
            if (keys[i] && !held(s->keys, keys[i]))
                emit(ctx, HID_KBD_PAGE_KEYBOARD, keys[i], 1);
        s->modifiers = mods;
        memcpy(s->keys, keys, sizeof(keys));
        return HID_KBD_EVENTS;
    }
    if (r[0] == HID_KBD_REPORT_CONSUMER) {
        uint16_t usage;
        if (len != HID_KBD_CONSUMER_BYTES)
            return HID_KBD_BAD_LENGTH;
        usage = (uint16_t)(r[1] | r[2] << 8);
        if (usage == s->consumer)
            return HID_KBD_EVENTS;
        if (s->consumer)
            emit(ctx, HID_KBD_PAGE_CONSUMER, s->consumer, 0);
        if (usage)
            emit(ctx, HID_KBD_PAGE_CONSUMER, usage, 1);
        s->consumer = usage;
        return HID_KBD_EVENTS;
    }
    return HID_KBD_IGNORED;
}

void hid_kbd_release_all(struct hid_kbd_state *s, hid_kbd_emit_fn emit, void *ctx)
{
    if (!s || !emit)
        return;
    for (unsigned b = 0; b < 8; b++)
        if (s->modifiers & (1u << b))
            emit(ctx, HID_KBD_PAGE_KEYBOARD, 0xe0u + b, 0);
    for (unsigned i = 0; i < 6; i++)
        if (s->keys[i])
            emit(ctx, HID_KBD_PAGE_KEYBOARD, s->keys[i], 0);
    if (s->consumer)
        emit(ctx, HID_KBD_PAGE_CONSUMER, s->consumer, 0);
    memset(s, 0, sizeof(*s));
}

#ifndef SNAP_I2C_HID_FNKEYS
#define SNAP_I2C_HID_FNKEYS 0
#endif
#if SNAP_I2C_HID_FNKEYS
/* The Fn-key layer: contract, evidence and labels in hid-keyboard.h. */

#define KEY_F1          0x3au
#define KEY_F12         0x45u
#define KEY_F13         0x68u
#define KEY_F24         0x73u
/* Report 54 keys the raw log may name: F1-F12, Print Screen, Scroll Lock, Pause, Insert, Home,
 * Page Up, Delete, End, Page Down, the arrows and Num Lock. None of them types a character. */
#define BAND_FIRST      0x3au
#define BAND_LAST       0x53u
#define MODS_GUI        0x88u          /* Left GUI (0xE3) and Right GUI (0xE7) in the modifier byte */
#define VENDOR_FN_ESC   0x4eu
#define BRIGHTNESS      0xfffeu        /* frow page: the encoding fn->flags selects; usage 1 = up */

/* The F-row: each key's hotkey as report 90 or report 55 sends it, and what macOS's keyboards send
 * for that function (0/0: none). Codes and functions are labelled in hid-keyboard.h. */
static const struct frow_key {
    uint8_t vendor[2];
    uint16_t consumer;
    uint16_t page, usage;
} frow[12] = {
    {{0x00, 0x00}, 0x00e2, HID_KBD_PAGE_CONSUMER, 0x00e2},                  /* F1  speaker on/off */
    {{0x00, 0x00}, 0x00ea, HID_KBD_PAGE_CONSUMER, 0x00ea},                  /* F2  volume down */
    {{0x00, 0x00}, 0x00e9, HID_KBD_PAGE_CONSUMER, 0x00e9},                  /* F3  volume up */
    {{0xc7, 0x00}, 0x0000, HID_FNKEYS_PAGE_APPLE_TOPCASE, 0x0007},          /* F4  keyboard light */
    {{0x10, 0x00}, 0x0070, BRIGHTNESS, 0},                                  /* F5  brightness down */
    {{0x20, 0x00}, 0x006f, BRIGHTNESS, 1},                                  /* F6  brightness up */
    {{0x00, 0x00}, 0x0000, HID_FNKEYS_PAGE_APPLE_TOPCASE, 0x0006},          /* F7  display mode */
    {{0x00, 0x00}, 0x0000, 0, 0},                                           /* F8  screen capture */
    {{0x6b, 0x00}, 0x0000, 0, 0},                                           /* F9  touchpad on/off */
    {{0x7c, 0x00}, 0x0000, HID_FNKEYS_PAGE_GENERIC_DESKTOP, 0x00a9},        /* F10 microphone on/off */
    {{0x82, 0x85}, 0x0000, 0, 0},                                           /* F11 camera on/off */
    {{0x86, 0x00}, 0x0000, 0, 0},                                           /* F12 MyASUS */
};

/* Report-90 codes off the F-row that have a macOS usage. */
static const struct { uint8_t vendor; uint16_t page, usage; } other_vendor[] = {
    {0xc4, HID_FNKEYS_PAGE_APPLE_TOPCASE, 0x0008},                          /* keyboard light up */
    {0xc5, HID_FNKEYS_PAGE_APPLE_TOPCASE, 0x0009},                          /* keyboard light down */
};

struct fn_call {
    struct hid_fnkeys *fn;
    hid_kbd_emit_fn emit;
    void *ctx;
    hid_fnkeys_note_fn note;
    void *note_ctx;
    uint8_t mods;                      /* report 54's modifier byte, for the notes */
};

static int contains(const uint8_t *a, unsigned n, uint8_t v)
{
    for (unsigned i = 0; i < n; i++)
        if (a[i] == v)
            return 1;
    return 0;
}

int hid_fnkeys_init(struct hid_fnkeys *fn, uint32_t flags)
{
    if (!fn)
        return HID_FNKEYS_REFUSED_BITS;
    memset(fn, 0, sizeof(*fn));
    if (flags & ~HID_FNKEYS_KNOWN)
        return HID_FNKEYS_REFUSED_BITS;
    if ((flags & HID_FNKEYS_BRIGHTNESS_MASK) >> HID_FNKEYS_BRIGHTNESS_SHIFT > HID_FNKEYS_BRIGHTNESS_CONSUMER)
        return HID_FNKEYS_REFUSED_ENCODING;
    if ((flags & HID_FNKEYS_HOST_LOCK) && !(flags & HID_FNKEYS_TRANSLATE))
        return HID_FNKEYS_REFUSED_LOCK;
    if (flags & (HID_FNKEYS_TRANSLATE | HID_FNKEYS_RAW_LOG))
        fn->flags = flags;
    return HID_FNKEYS_OK;
}

const char *hid_fnkeys_init_name(int result)
{
    switch (result) {
    case HID_FNKEYS_OK: return "accepted";
    case HID_FNKEYS_REFUSED_BITS: return "unknown bits";
    case HID_FNKEYS_REFUSED_ENCODING: return "brightness encoding 3";
    case HID_FNKEYS_REFUSED_LOCK: return "host lock without translate";
    default: return "unknown result";
    }
}

void hid_fnkeys_brightness(const struct hid_fnkeys *fn, int up, uint16_t *page, uint16_t *usage)
{
    switch (fn ? (fn->flags & HID_FNKEYS_BRIGHTNESS_MASK) >> HID_FNKEYS_BRIGHTNESS_SHIFT : 0u) {
    case HID_FNKEYS_BRIGHTNESS_APPLE_KEYBOARD:
        *page = HID_FNKEYS_PAGE_APPLE_KEYBOARD;
        *usage = up ? 0x0020u : 0x0021u;
        break;
    case HID_FNKEYS_BRIGHTNESS_CONSUMER:
        *page = HID_KBD_PAGE_CONSUMER;
        *usage = up ? 0x006fu : 0x0070u;
        break;
    default:
        *page = HID_FNKEYS_PAGE_APPLE_TOPCASE;
        *usage = up ? 0x0004u : 0x0005u;
        break;
    }
}

static void function_of(const struct hid_fnkeys *fn, const struct frow_key *k, uint16_t *page, uint16_t *usage)
{
    if (k->page == BRIGHTNESS) {
        hid_fnkeys_brightness(fn, k->usage, page, usage);
    } else {
        *page = k->page;
        *usage = k->usage;
    }
}

static const struct frow_key *frow_for(uint16_t page, uint16_t usage)
{
    if (!usage)
        return NULL;
    for (unsigned i = 0; i < 12; i++) {
        if (page == HID_FNKEYS_PAGE_ASUS && (frow[i].vendor[0] == usage || frow[i].vendor[1] == usage))
            return &frow[i];
        if (page == HID_KBD_PAGE_CONSUMER && frow[i].consumer == usage)
            return &frow[i];
    }
    return NULL;
}

/* What a key dispatches as with the host lock as it is now (0/0: nothing). Without TRANSLATE every
 * key is itself and report 90 dispatches nothing, which is the driver without the layer. */
static void remap(const struct hid_fnkeys *fn, uint16_t page, uint16_t usage, uint16_t *op, uint16_t *ou)
{
    const int translate = (fn->flags & HID_FNKEYS_TRANSLATE) != 0;
    const int locked = translate && (fn->flags & HID_FNKEYS_HOST_LOCK) && fn->lock;
    const struct frow_key *k;

    *op = page == HID_FNKEYS_PAGE_ASUS ? 0u : page;
    *ou = page == HID_FNKEYS_PAGE_ASUS ? 0u : usage;
    if (!translate)
        return;
    if (page == HID_KBD_PAGE_KEYBOARD) {
        if (locked && usage >= KEY_F1 && usage <= KEY_F12)
            function_of(fn, &frow[usage - KEY_F1], op, ou);
        return;
    }
    k = frow_for(page, usage);
    if (k) {
        if (locked) {
            *op = HID_KBD_PAGE_KEYBOARD;
            *ou = (uint16_t)(KEY_F1 + (unsigned)(k - frow));
        } else {
            function_of(fn, k, op, ou);
        }
        return;
    }
    if (page == HID_FNKEYS_PAGE_ASUS)
        for (unsigned i = 0; i < sizeof(other_vendor) / sizeof(other_vendor[0]); i++)
            if (other_vendor[i].vendor == usage) {
                *op = other_vendor[i].page;
                *ou = other_vendor[i].usage;
            }
}

static struct hid_fnkeys_held *find_held(struct hid_fnkeys *fn, uint16_t page, uint16_t usage)
{
    for (unsigned i = 0; i < HID_FNKEYS_HELD; i++)
        if (fn->held[i].in_page == page && fn->held[i].in_usage == usage)
            return &fn->held[i];
    return NULL;
}

static void note(const struct fn_call *c, uint8_t kind, uint8_t id, uint8_t down, uint16_t code, uint16_t op,
                 uint16_t ou, const uint8_t *raw, uint32_t raw_len)
{
    struct hid_fnkeys_note n;
    if (!c->note || !(c->fn->flags & HID_FNKEYS_RAW_LOG))
        return;
    memset(&n, 0, sizeof(n));
    n.kind = kind;
    n.id = id;
    n.down = down;
    n.mods = c->mods;
    n.code = code;
    n.out_page = op;
    n.out_usage = ou;
    n.raw = raw;
    n.raw_len = raw ? raw_len : 0u;
    c->note(c->note_ctx, &n);
}

/* A press records what it dispatched; its release dispatches the same, whatever the lock did in
 * between. Every input page is non-zero, so in_page 0 marks a free slot; at most 12 keys are down
 * at once (6 + 1 + 5), so a press always finds one. */
static void press(struct fn_call *c, uint16_t page, uint16_t usage, uint16_t *op, uint16_t *ou)
{
    struct hid_fnkeys_held *h = find_held(c->fn, page, usage);
    if (h) {                           /* already down: nothing new */
        *op = h->out_page;
        *ou = h->out_usage;
        return;
    }
    remap(c->fn, page, usage, op, ou);
    h = find_held(c->fn, 0, 0);
    if (h) {
        h->in_page = page;
        h->in_usage = usage;
        h->out_page = *op;
        h->out_usage = *ou;
    }
    if (*op || *ou)
        c->emit(c->ctx, *op, *ou, 1);
}

/* A release without a recorded press dispatches nothing: never an up for a down macOS did not see. */
static void release(struct fn_call *c, uint16_t page, uint16_t usage, uint16_t *op, uint16_t *ou)
{
    struct hid_fnkeys_held *h = find_held(c->fn, page, usage);
    *op = *ou = 0;
    if (!h)
        return;
    *op = h->out_page;
    *ou = h->out_usage;
    memset(h, 0, sizeof(*h));
    if (*op || *ou)
        c->emit(c->ctx, *op, *ou, 0);
}

/* hid_kbd_translate's transitions of reports 54 and 55, dispatched through the layer. */
static void through(void *ctx, uint32_t page, uint32_t usage, uint32_t down)
{
    struct fn_call *c = ctx;
    uint16_t op, ou;
    if (page == HID_KBD_PAGE_KEYBOARD && usage >= 0xe0u && usage <= 0xe7u) {
        c->emit(c->ctx, page, usage, down);
        return;
    }
    if (down)
        press(c, (uint16_t)page, (uint16_t)usage, &op, &ou);
    else
        release(c, (uint16_t)page, (uint16_t)usage, &op, &ou);
    if (page == HID_KBD_PAGE_CONSUMER)
        note(c, HID_FNKEYS_NOTE_CONSUMER, HID_KBD_REPORT_CONSUMER, (uint8_t)down, (uint16_t)usage, op, ou, NULL, 0);
    else if (usage >= BAND_FIRST && usage <= BAND_LAST)
        note(c, HID_FNKEYS_NOTE_KEY, HID_KBD_REPORT_KEYS, (uint8_t)down, (uint16_t)usage, op, ou, NULL, 0);
}

/* The device's own transitions, on fn->shadow: the raw log's source while an event driver
 * dispatches reports 54 and 55 (out 0/0: not ours to dispatch). */
static void observe(void *ctx, uint32_t page, uint32_t usage, uint32_t down)
{
    struct fn_call *c = ctx;
    if (!c->fn->yielded)
        return;
    if (page == HID_KBD_PAGE_CONSUMER)
        note(c, HID_FNKEYS_NOTE_CONSUMER, HID_KBD_REPORT_CONSUMER, (uint8_t)down, (uint16_t)usage, 0, 0, NULL, 0);
    else if (page == HID_KBD_PAGE_KEYBOARD && usage >= BAND_FIRST && usage <= BAND_LAST)
        note(c, HID_FNKEYS_NOTE_KEY, HID_KBD_REPORT_KEYS, (uint8_t)down, (uint16_t)usage, 0, 0, NULL, 0);
}

/* What hid_kbd_translate drops from report 54 and the raw log still names: F13-F24 (past the
 * descriptor's logical maximum 0x65; Windows' Copilot key is Shift+GUI+F23) and a GUI modifier
 * going down with a new key in one report, which is how firmware sends Win+P or Win+Shift+S. */
static void observe_keys(struct fn_call *c, const uint8_t *r, uint32_t len)
{
    struct hid_fnkeys *fn = c->fn;
    uint8_t range[6] = {0, 0, 0, 0, 0, 0};
    unsigned n = 0;
    if (!(fn->flags & HID_FNKEYS_RAW_LOG) || len != HID_KBD_KEYS_BYTES || contains(r + 3, 6, HID_KBD_ROLLOVER))
        return;
    for (unsigned i = 0; i < 6; i++)
        if (r[3 + i] >= KEY_F13 && r[3 + i] <= KEY_F24 && !contains(range, n, r[3 + i]))
            range[n++] = r[3 + i];
    for (unsigned i = 0; i < 6; i++)
        if (fn->range[i] && !contains(range, n, fn->range[i]))
            note(c, HID_FNKEYS_NOTE_RANGE, HID_KBD_REPORT_KEYS, 0, fn->range[i], 0, 0, NULL, 0);
    for (unsigned i = 0; i < n; i++)
        if (!contains(fn->range, 6, range[i]))
            note(c, HID_FNKEYS_NOTE_RANGE, HID_KBD_REPORT_KEYS, 1, range[i], 0, 0, NULL, 0);
    memcpy(fn->range, range, sizeof(range));
    if (!((uint32_t)r[1] & MODS_GUI & ~(uint32_t)fn->shadow.modifiers))
        return;
    for (unsigned i = 0; i < 6; i++)
        if (r[3 + i] >= 0x04u && r[3 + i] <= HID_KBD_MAX_KEY && !contains(fn->shadow.keys, 6, r[3 + i]))
            note(c, HID_FNKEYS_NOTE_CHORD, HID_KBD_REPORT_KEYS, 1, r[3 + i], 0, 0, NULL, 0);
}

/* Report 90: an array of codes down now; codes that left are released, then new ones pressed.
 * 0x00 and 0xFF are no code (hid-asus treats 0xFF as none too). */
static int vendor_report(struct fn_call *c, const uint8_t *r, uint32_t len)
{
    struct hid_fnkeys *fn = c->fn;
    uint8_t now[HID_FNKEYS_VENDOR_SLOTS] = {0, 0, 0, 0, 0};
    unsigned n = 0;
    uint16_t op, ou;

    if (len != HID_KBD_VENDOR_BYTES) {
        note(c, HID_FNKEYS_NOTE_REPORT, r[0], 0, (uint16_t)len, 0, 0, r, len);
        return HID_KBD_BAD_LENGTH;
    }
    fn->vendor_reports++;
    for (unsigned i = 0; i < HID_FNKEYS_VENDOR_SLOTS; i++)
        if (r[1 + i] && r[1 + i] != 0xffu && !contains(now, n, r[1 + i]))
            now[n++] = r[1 + i];
    for (unsigned i = 0; i < HID_FNKEYS_VENDOR_SLOTS; i++) {
        uint8_t v = fn->vendor[i];
        if (!v || contains(now, n, v))
            continue;
        release(c, HID_FNKEYS_PAGE_ASUS, v, &op, &ou);
        note(c, HID_FNKEYS_NOTE_VENDOR, r[0], 0, v, op, ou, r, len);
    }
    for (unsigned i = 0; i < n; i++) {
        uint8_t v = now[i];
        if (contains(fn->vendor, HID_FNKEYS_VENDOR_SLOTS, v))
            continue;
        if (v == VENDOR_FN_ESC && (fn->flags & HID_FNKEYS_HOST_LOCK)) {
            fn->lock ^= 1u;
            fn->lock_toggles++;
            note(c, HID_FNKEYS_NOTE_LOCK, r[0], fn->lock, v, 0, 0, NULL, 0);
        }
        press(c, HID_FNKEYS_PAGE_ASUS, v, &op, &ou);
        if (!op && !ou)
            fn->unmapped++;
        note(c, HID_FNKEYS_NOTE_VENDOR, r[0], 1, v, op, ou, r, len);
    }
    memcpy(fn->vendor, now, sizeof(now));
    return HID_KBD_EVENTS;
}

int hid_fnkeys_translate(struct hid_fnkeys *fn, struct hid_kbd_state *kbd, const uint8_t *r, uint32_t len,
                         hid_kbd_emit_fn emit, void *ctx, hid_fnkeys_note_fn note_fn, void *note_ctx)
{
    struct fn_call c = {fn, emit, ctx, note_fn, note_ctx, 0};
    int result;

    if (!fn || !kbd || !r || !len || !emit)
        return HID_KBD_BAD_LENGTH;
    if (r[0] == HID_KBD_REPORT_VENDOR)
        return vendor_report(&c, r, len);
    if (r[0] != HID_KBD_REPORT_KEYS && r[0] != HID_KBD_REPORT_CONSUMER) {
        note(&c, HID_FNKEYS_NOTE_REPORT, r[0], 0, (uint16_t)len, 0, 0, r, len);
        return HID_KBD_IGNORED;
    }
    if (r[0] == HID_KBD_REPORT_KEYS && len >= 2u) {
        c.mods = r[1];
        observe_keys(&c, r, len);
    }
    result = hid_kbd_translate(&fn->shadow, r, len, observe, &c);
    if (fn->yielded)
        return result == HID_KBD_EVENTS ? HID_KBD_IGNORED : result;
    return hid_kbd_translate(kbd, r, len, through, &c);
}

void hid_fnkeys_release_keyboard(struct hid_fnkeys *fn, struct hid_kbd_state *kbd, hid_kbd_emit_fn emit, void *ctx)
{
    struct fn_call c = {fn, emit, ctx, NULL, NULL, 0};
    if (fn && kbd && emit)
        hid_kbd_release_all(kbd, through, &c);
}

void hid_fnkeys_release_all(struct hid_fnkeys *fn, struct hid_kbd_state *kbd, hid_kbd_emit_fn emit, void *ctx)
{
    struct fn_call c = {fn, emit, ctx, NULL, NULL, 0};
    uint16_t op, ou;
    if (!fn || !kbd || !emit)
        return;
    hid_kbd_release_all(kbd, through, &c);
    for (unsigned i = 0; i < HID_FNKEYS_VENDOR_SLOTS; i++)
        if (fn->vendor[i]) {
            release(&c, HID_FNKEYS_PAGE_ASUS, fn->vendor[i], &op, &ou);
            fn->vendor[i] = 0;
        }
}

int hid_fnkeys_rate_allow(struct hid_fnkeys_rate *rate, uint64_t now_us)
{
    if (!rate)
        return 0;
    if (!rate->started) {
        rate->started = 1;
        rate->tokens = HID_FNKEYS_RATE_BURST;
        rate->refilled_us = now_us;
    } else if (now_us > rate->refilled_us) {
        uint64_t add = (now_us - rate->refilled_us) / HID_FNKEYS_RATE_REFILL_US;
        if (add) {
            rate->tokens = add >= HID_FNKEYS_RATE_BURST - rate->tokens ? HID_FNKEYS_RATE_BURST
                                                                      : rate->tokens + (uint32_t)add;
            rate->refilled_us += add * HID_FNKEYS_RATE_REFILL_US;
        }
    }
    if (!rate->tokens || rate->lines >= HID_FNKEYS_RATE_LIFETIME) {
        rate->dropped++;
        return 0;
    }
    rate->tokens--;
    rate->lines++;
    return 1;
}
#endif
