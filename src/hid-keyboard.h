/* SPDX-License-Identifier: MIT */
/*
 * Report-to-key translation for SnapI2CHIDKeyboard, our own IOHIDEventService
 * (src/SnapI2CHID.cpp). 26A428's generic keyboard drivers are all IOMatchDefer
 * and none attached in v40/v41 (docs/A16-INPUT-INSTALLED-20261017.md sections
 * 11-12), so the kext turns the A16 keyboard's reports into key events itself.
 *
 * Layouts, from the keyboard's 144-byte report descriptor (SHA256 0887190d...):
 *   report 54 (9 bytes): ID, modifier bits E0..E7, a constant byte, six key
 *                        usages (page 7, array, logical 0..0x65);
 *   report 55 (3 bytes): ID, one 16-bit Consumer usage (array, 0 = none).
 * Report 90 (ASUS vendor page) and anything else produce no event here; the opt-in Fn-key layer
 * at the end of this header takes report 90.
 *
 * Hardware independent, like the rest of src/: the host test compiles it.
 */
#ifndef SNAP_HID_KEYBOARD_H
#define SNAP_HID_KEYBOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HID_KBD_REPORT_KEYS      0x36u
#define HID_KBD_REPORT_CONSUMER  0x37u
#define HID_KBD_KEYS_BYTES       9u
#define HID_KBD_CONSUMER_BYTES   3u
#define HID_KBD_PAGE_KEYBOARD    0x07u
#define HID_KBD_PAGE_CONSUMER    0x0cu
#define HID_KBD_MAX_KEY          0x65u   /* the array's logical maximum */
#define HID_KBD_ROLLOVER         0x01u   /* ErrorRollOver: the whole report is unusable */

struct hid_kbd_state {
    uint8_t modifiers;
    uint8_t keys[6];
    uint16_t consumer;
};

/* One key transition: down 1 or 0. */
typedef void (*hid_kbd_emit_fn)(void *ctx, uint32_t page, uint32_t usage, uint32_t down);

enum hid_kbd_result { HID_KBD_EVENTS = 0, HID_KBD_IGNORED, HID_KBD_ROLLOVER_REPORT, HID_KBD_BAD_LENGTH };

/* Emit the transitions from *state to this report, in a fixed order (modifier
 * releases, key releases, modifier presses, key presses; consumer release then
 * press), and update *state. Returns how the report was taken. */
int hid_kbd_translate(struct hid_kbd_state *state, const uint8_t *report, uint32_t len,
                      hid_kbd_emit_fn emit, void *ctx);
/* Release everything held (stop, or a re-initialised device). */
void hid_kbd_release_all(struct hid_kbd_state *state, hid_kbd_emit_fn emit, void *ctx);

/*
 * Fn-key layer, compiled in with SNAP_I2C_HID_FNKEYS and turned on by SnapI2CHIDKeyboard from the
 * boot-arg snap_i2c_fnkeys (HID_FNKEYS_* bits). Without both, hid_kbd_translate above is the whole
 * keyboard path, as before.
 *
 * The descriptor's third collection is ASUS vendor page 0xFF31 usage 0x76 (VERIFIED, the 144 bytes
 * above): input report 90, 6 bytes (ID and five 8-bit vendor codes, an array, 0 = none), and a
 * 64-byte feature report 90. On Windows the ASUS hotkey service opens that collection
 * (HID_DEVICE_UP:FF31_U:0076) and hands each code to ACPI; the A16's DSDT names the codes it takes
 * (ATKD.WMNB, DEVS 0x00100021 "Fn-key simulation"): 0x10 brightness down, 0x20 brightness up, 0x6B,
 * 0xC5, 0xC7, 0x7C, 0xCB, 0x4E, 0x86, 0x7E, 0x8B, 0x9D, 0x5F (OBSERVED). Linux's hid-asus maps the
 * same page the same way (0x10/0x20 brightness, 0x7C microphone mute, 0x82/0x85 camera, 0x86 MyASUS,
 * 0xC4/0xC5/0xC7 keyboard light up/down/toggle, 0x6B touchpad toggle, 0x4E Fn+Esc). Which F-key
 * sends which code is INFERRED from the UX3607 manual's hotkey list (F1 speaker, F2/F3 volume, F4
 * keyboard light, F5/F6 brightness, F7 display mode, F8 screen capture, F9 touchpad, F10
 * microphone, F11 camera, F12 MyASUS) until the raw log of an A16 run names them.
 *
 * Out, what macOS's own keyboards send for those functions: brightness as Apple vendor top-case
 * 0x00FF/0x0005 (down) and 0x0004 (up), the usages the 26A428 keyboard filter gives the MacBook
 * keyboard's F1/F2 (its FnFunctionUsageMap, read on the 26A428 host: VERIFIED); volume and mute as
 * Consumer 0xEA/0xE9/0xE2 (same map: VERIFIED); keyboard light as top-case 0x0007/0x0008/0x0009 and
 * display mode as top-case 0x0006 (Apple's AppleHIDUsageTables.h, not in the SDK: INFERRED);
 * microphone mute as Generic Desktop 0x00A9 (IOHIDUsageTables.h). Touchpad, camera, MyASUS and
 * screen capture have no macOS usage: nothing is dispatched for them.
 *
 * Fn+Esc: the DSDT keeps the Fn-lock in the EC (DEVS 0x00100023 "Fn-key Switch", an EC command on
 * another bus), so on Windows the host flips it after the keyboard reports 0x4E (INFERRED). The guest
 * writes nothing to the EC; HID_FNKEYS_HOST_LOCK instead swaps, in the host, the F-row's hotkeys and
 * F1-F12 on each 0x4E. Leave it off if the keyboard turns out to swap them itself.
 */
#define HID_KBD_REPORT_VENDOR        0x5au
#define HID_KBD_VENDOR_BYTES         6u

#define HID_FNKEYS_TRANSLATE         0x01u   /* report 90 and report 55's brightness to macOS's usages */
#define HID_FNKEYS_RAW_LOG           0x02u   /* notes (below) on what the F-row and unknown reports send */
#define HID_FNKEYS_HOST_LOCK         0x04u   /* Fn+Esc swaps F1-F12 and their hotkeys here; needs TRANSLATE */
#define HID_FNKEYS_BRIGHTNESS_SHIFT  3u      /* two bits: the brightness encoding, HID_FNKEYS_BRIGHTNESS_* */
#define HID_FNKEYS_BRIGHTNESS_MASK   (3u << HID_FNKEYS_BRIGHTNESS_SHIFT)
#define HID_FNKEYS_KNOWN             (HID_FNKEYS_TRANSLATE | HID_FNKEYS_RAW_LOG | HID_FNKEYS_HOST_LOCK | \
                                      HID_FNKEYS_BRIGHTNESS_MASK)
enum {
    HID_FNKEYS_BRIGHTNESS_TOPCASE = 0,       /* 0x00FF/0x0005 down, 0x0004 up (the MacBook keyboard's) */
    HID_FNKEYS_BRIGHTNESS_APPLE_KEYBOARD,    /* 0xFF01/0x0021 down, 0x0020 up (the 26A428 host's other map) */
    HID_FNKEYS_BRIGHTNESS_CONSUMER,          /* 0x000C/0x0070 down, 0x006F up */
};
enum hid_fnkeys_init_result {
    HID_FNKEYS_OK = 0, HID_FNKEYS_REFUSED_BITS, HID_FNKEYS_REFUSED_ENCODING, HID_FNKEYS_REFUSED_LOCK
};

#define HID_FNKEYS_PAGE_GENERIC_DESKTOP  0x0001u
#define HID_FNKEYS_PAGE_APPLE_TOPCASE    0x00ffu
#define HID_FNKEYS_PAGE_APPLE_KEYBOARD   0xff01u
#define HID_FNKEYS_PAGE_ASUS             0xff31u   /* report 90's codes; never dispatched */
#define HID_FNKEYS_VENDOR_SLOTS          5u
#define HID_FNKEYS_HELD                  16u       /* 6 keys + 1 consumer + 5 vendor codes, with room */

/* A key that is down, and what it was dispatched as when it went down (0/0: nothing), so its
 * release matches its press even if the host lock flipped in between. */
struct hid_fnkeys_held {
    uint16_t in_page, in_usage, out_page, out_usage;
};

struct hid_fnkeys {
    uint32_t flags;
    uint8_t lock;                    /* HID_FNKEYS_HOST_LOCK: 1 = F1-F12 first, hotkeys with Fn */
    uint8_t yielded;                 /* an event driver dispatches reports 54 and 55: only report 90 here */
    uint8_t vendor[HID_FNKEYS_VENDOR_SLOTS];   /* report 90's codes down now */
    uint8_t range[6];                /* report 54's F13-F24 values down now (raw log) */
    struct hid_kbd_state shadow;     /* reports 54 and 55 as the device sent them (raw log, also while yielded) */
    struct hid_fnkeys_held held[HID_FNKEYS_HELD];
    uint32_t vendor_reports, unmapped, lock_toggles;
};

/* Raw-log notes. Report 54 is named only for keys that type no character: the F-row band
 * 0x3A..0x53, F13-F24, and a GUI modifier going down with a key in the same report (a firmware
 * chord such as Win+P); its bytes are never passed. */
enum hid_fnkeys_note_kind {
    HID_FNKEYS_NOTE_VENDOR = 1,      /* a report-90 code went down or up; out = what it dispatched */
    HID_FNKEYS_NOTE_CONSUMER,        /* a report-55 usage went down or up */
    HID_FNKEYS_NOTE_KEY,             /* a report-54 key of the F-row band went down or up */
    HID_FNKEYS_NOTE_RANGE,           /* report 54 carried F13-F24, past the descriptor's logical maximum */
    HID_FNKEYS_NOTE_CHORD,           /* GUI and a key went down in one report 54 */
    HID_FNKEYS_NOTE_REPORT,          /* an input report no layer takes, or report 90 at another length */
    HID_FNKEYS_NOTE_LOCK,            /* the host lock flipped; down = its new state */
};
struct hid_fnkeys_note {
    uint8_t kind, id, down, mods;
    uint16_t code, out_page, out_usage;
    const uint8_t *raw;              /* NOTE_VENDOR and NOTE_REPORT of IDs other than 54 only */
    uint32_t raw_len;
};
typedef void (*hid_fnkeys_note_fn)(void *ctx, const struct hid_fnkeys_note *note);

/* Clear *fn and take flags; a refused set leaves fn->flags 0 (off). Flags with neither TRANSLATE
 * nor RAW_LOG are off too (HID_FNKEYS_OK, fn->flags 0). */
int hid_fnkeys_init(struct hid_fnkeys *fn, uint32_t flags);
const char *hid_fnkeys_init_name(int result);
/* The brightness usages fn->flags selects. */
void hid_fnkeys_brightness(const struct hid_fnkeys *fn, int up, uint16_t *page, uint16_t *usage);
/* hid_kbd_translate with the layer: same results and same events for everything it does not
 * change. While fn->yielded only report 90 dispatches. note (may be NULL) gets the raw-log notes
 * when fn->flags has HID_FNKEYS_RAW_LOG. Requires fn->flags != 0. */
int hid_fnkeys_translate(struct hid_fnkeys *fn, struct hid_kbd_state *kbd, const uint8_t *report, uint32_t len,
                         hid_kbd_emit_fn emit, void *ctx, hid_fnkeys_note_fn note, void *note_ctx);
/* Release what reports 54 and 55 hold (before yielding), or everything (stop). */
void hid_fnkeys_release_keyboard(struct hid_fnkeys *fn, struct hid_kbd_state *kbd, hid_kbd_emit_fn emit, void *ctx);
void hid_fnkeys_release_all(struct hid_fnkeys *fn, struct hid_kbd_state *kbd, hid_kbd_emit_fn emit, void *ctx);

/* The raw log's rate limit: a burst of HID_FNKEYS_RATE_BURST lines, then one per
 * HID_FNKEYS_RATE_REFILL_US, at most HID_FNKEYS_RATE_LIFETIME a boot. Lines refused are counted in
 * dropped; the caller reports and clears it with the next line it may print. */
#define HID_FNKEYS_RATE_BURST      32u
#define HID_FNKEYS_RATE_REFILL_US  125000u
#define HID_FNKEYS_RATE_LIFETIME   4096u
struct hid_fnkeys_rate {
    uint64_t refilled_us;
    uint32_t tokens, lines, dropped;
    uint8_t started;
};
int hid_fnkeys_rate_allow(struct hid_fnkeys_rate *rate, uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif
