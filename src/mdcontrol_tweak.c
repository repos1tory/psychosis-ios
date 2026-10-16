// MDControl.dylib - system-wide input server, runs inside SpringBoard.
// No hooks, no private headers: constructor starts a localhost TCP server,
// dispatches HID events from SpringBoard context (Veency-proven path).
// Protocol (one bounded, newline-terminated request per connection):
//   ping | tap X Y [W H] | swipe X1 Y1 X2 Y2 [steps [W H]]
//   home | volup | launch BUNDLE_ID | ax [depth]
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "mdc_protocol.h"
#include "mdc_overlay.h"
#include "mdc_services.h"
#include "mdc_keymap.h"

int ax_find_center(const char *label, double *out_x, double *out_y);
int ax_front_info(char *bundle, size_t cap, int *pid, int *system_root);

typedef void *CFRef;
typedef double IOHIDFloat;
typedef int Boolean;

static CFRef (*P_CreateClient)(void *);
static void (*P_Dispatch)(CFRef, CFRef);
static void (*P_SetMatching)(CFRef, CFRef);
static CFRef (*P_CreateHand)(void *, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                             IOHIDFloat, IOHIDFloat, IOHIDFloat, IOHIDFloat, IOHIDFloat,
                             Boolean, Boolean, uint32_t);
static CFRef (*P_CreateFinger)(void *, uint64_t, uint32_t, uint32_t, uint32_t,
                               IOHIDFloat, IOHIDFloat, IOHIDFloat, IOHIDFloat, IOHIDFloat,
                               Boolean, Boolean, uint32_t);
static CFRef (*P_CreateKey)(void *, uint64_t, uint16_t, uint16_t, Boolean, uint32_t);
static void (*P_Append)(CFRef, CFRef);
static void (*P_SetInt)(CFRef, uint32_t, int);
static void (*P_SetFloat)(CFRef, uint32_t, double);
static void (*P_SetSender)(CFRef, uint64_t);
static uint64_t (*P_AbsTime)(void);
static void (*P_CFRelease)(CFRef);
static CFRef (*P_CreateUnicode)(void *, uint64_t, const uint8_t *, uint32_t, uint32_t, uint32_t);
static CFRef g_client = 0;
static int g_hid_ready = 0;

#define T_HAND 3
#define EV_RANGE 1
#define EV_TOUCH 2
#define EV_POS 4
#define EV_ID 32
#define F_BUILTIN 4
#define F_DISP_INT ((11u << 16) + 25)
#define F_MAJOR ((11u << 16) + 20)
#define F_MINOR ((11u << 16) + 21)
#define SENDER 0x8000000817319372ull // TrollVNC: iOS 14.8-26 (0xDEFA... dead)
#define HID_PAGE_CONSUMER 0x0C
#define HID_USE_MENU 0x40
#define HID_USE_VOLUP 0xE9

static int hid_init(void) {
    void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (!h) return -1;
    P_CreateClient = dlsym(h, "IOHIDEventSystemClientCreate");
    P_Dispatch = dlsym(h, "IOHIDEventSystemClientDispatchEvent");
    P_SetMatching = dlsym(h, "IOHIDEventSystemClientSetMatching");
    P_CreateHand = dlsym(h, "IOHIDEventCreateDigitizerEvent");
    P_CreateFinger = dlsym(h, "IOHIDEventCreateDigitizerFingerEvent");
    P_CreateKey = dlsym(h, "IOHIDEventCreateKeyboardEvent");
    P_Append = dlsym(h, "IOHIDEventAppendEvent");
    P_SetInt = dlsym(h, "IOHIDEventSetIntegerValue");
    P_SetFloat = dlsym(h, "IOHIDEventSetFloatValue");
    P_SetSender = dlsym(h, "IOHIDEventSetSenderID");
    P_CreateUnicode = dlsym(h, "IOHIDEventCreateUnicodeEvent");
    if (!P_CreateUnicode) P_CreateUnicode = dlsym(h, "_IOHIDEventCreateUnicodeEvent");
    void *sys = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY);
    P_AbsTime = dlsym(sys, "mach_absolute_time");
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
    P_CFRelease = dlsym(cf, "CFRelease");
    if (!P_CreateClient || !P_Dispatch || !P_SetMatching || !P_CreateHand || !P_CreateFinger ||
        !P_CreateKey || !P_Append || !P_SetInt || !P_SetFloat || !P_SetSender || !P_AbsTime || !P_CFRelease)
        return -1;
    g_client = P_CreateClient(0);
    if (!g_client) return -1;
    P_SetMatching(g_client, 0);
    return 0;
}

// TrollVNC STHIDEventGenerator recipe: hand(Hand,0,0,mask), finger(2,2,mask),
// twist 90, radii 5, sender 0x...372. down/up mask = Touch|Identity (34),
// move mask = Position|Attribute (68).
static int send_phase(int phase, double nx, double ny) {
    if (!g_client || !P_CreateHand || !P_CreateFinger || !P_Append || !P_SetInt ||
        !P_SetFloat || !P_SetSender || !P_Dispatch || !P_CFRelease || !P_AbsTime) return -1;
    uint32_t m = (phase == 1) ? (EV_POS | (1u << 6)) : (EV_TOUCH | EV_ID);
    int touching = (phase != 2);
    CFRef hand = P_CreateHand(0, P_AbsTime(), T_HAND, 0, 0, m, 0,
                              0, 0, 0, 0, 0, 0, touching, 0);
    if (!hand) return -1;
    P_SetInt(hand, F_BUILTIN, 1);
    P_SetInt(hand, F_DISP_INT, 1);
    CFRef fg = P_CreateFinger(0, P_AbsTime(), 2, 2, m,
                              nx, ny, 0, 0.0, 90.0, touching, touching, 0);
    if (!fg) { P_CFRelease(hand); return -1; }
    P_SetFloat(fg, F_MINOR, 5.0);
    P_SetFloat(fg, F_MAJOR, 5.0);
    P_Append(hand, fg);
    P_CFRelease(fg);
    P_SetSender(hand, SENDER);
    P_Dispatch(g_client, hand);
    P_CFRelease(hand);
    return 0;
}

static int do_key(uint16_t usage) {
    if (!g_client || !P_CreateKey || !P_SetSender || !P_Dispatch || !P_CFRelease || !P_AbsTime)
        return -1;
    CFRef d = P_CreateKey(0, P_AbsTime(), HID_PAGE_CONSUMER, usage, 1, 0);
    if (!d) return -1;
    P_SetSender(d, SENDER); P_Dispatch(g_client, d); P_CFRelease(d);
    usleep(100000);
    CFRef u = P_CreateKey(0, P_AbsTime(), HID_PAGE_CONSUMER, usage, 0, 0);
    if (!u) return -1;
    P_SetSender(u, SENDER); P_Dispatch(g_client, u); P_CFRelease(u);
    return 0;
}

/* ---- wake / unlock ---- */

typedef void *OID;
typedef void *OSEL;

static void *g_tw_objc;
static void *g_tw_msg;
static OID (*tw_class)(const char *);
static OSEL (*tw_sel_reg)(const char *);

static int tw_rt_init(void) {
    if (g_tw_msg) return 0;
    g_tw_objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_tw_objc) return -1;
    tw_class = (OID (*)(const char *))dlsym(g_tw_objc, "objc_getClass");
    tw_sel_reg = (OSEL (*)(const char *))dlsym(g_tw_objc, "sel_registerName");
    g_tw_msg = dlsym(g_tw_objc, "objc_msgSend");
    return (tw_class && tw_sel_reg && g_tw_msg) ? 0 : -1;
}

#define HID_PAGE_KEYBOARD 0x07
#define HID_USAGE_ENTER 0x28
#define HID_USAGE_SHIFT_LEFT 0xE1

static int keyboard_event_raw(uint16_t usage, int down) {
    if (!g_client || !P_CreateKey || !P_SetSender || !P_Dispatch || !P_CFRelease || !P_AbsTime)
        return -1;
    CFRef e = P_CreateKey(0, P_AbsTime(), HID_PAGE_KEYBOARD, usage, down ? 1 : 0, 0);
    if (!e) return -1;
    P_SetSender(e, SENDER);
    P_Dispatch(g_client, e);
    P_CFRelease(e);
    return 0;
}

static int do_keyboard_key(uint16_t usage) {
    if (keyboard_event_raw(usage, 1) != 0) return -1;
    usleep(40000);
    if (keyboard_event_raw(usage, 0) != 0) return -1;
    usleep(120000);
    return 0;
}

/* ASCII per-character HID fallback (existing implementation). */
#define TYPE_DOWN_US 11000
#define TYPE_UP_GAP_US 9000
#define TYPE_SHIFT_GAP_US 6000

static int do_type_text_ascii(const char *text, size_t *out_typed) {
    if (out_typed) *out_typed = 0;
    if (!text || !text[0]) return -1;
    if (!g_client || !P_CreateKey || !P_SetSender || !P_Dispatch || !P_CFRelease || !P_AbsTime)
        return -1;
    size_t typed = 0;
    for (const char *p = text; *p; p++) {
        unsigned short usage = 0;
        int shift = 0;
        if (mdc_key_for_char(*p, &usage, &shift) != 0) {
            if (out_typed) *out_typed = typed;
            return -1;
        }
        if (shift) {
            if (keyboard_event_raw(HID_USAGE_SHIFT_LEFT, 1) != 0) {
                if (out_typed) *out_typed = typed;
                return -1;
            }
            usleep(TYPE_SHIFT_GAP_US);
        }
        if (keyboard_event_raw(usage, 1) != 0) {
            if (out_typed) *out_typed = typed;
            return -1;
        }
        usleep(TYPE_DOWN_US);
        if (keyboard_event_raw(usage, 0) != 0) {
            if (out_typed) *out_typed = typed;
            return -1;
        }
        if (shift) {
            usleep(TYPE_SHIFT_GAP_US);
            if (keyboard_event_raw(HID_USAGE_SHIFT_LEFT, 0) != 0) {
                if (out_typed) *out_typed = typed;
                return -1;
            }
        }
        usleep(TYPE_UP_GAP_US);
        typed++;
    }
    if (out_typed) *out_typed = typed;
    return 0;
}

/* Primary text path: IOHIDEventCreateUnicodeEvent with UTF-16LE payloads
 * (64 units per chunk). Handles any unicode text in a few events. */
#define UNICODE_CHUNK_UNITS 64

static int do_type_text_unicode(const char *text) {
    if (!g_client || !P_CreateUnicode || !P_SetInt || !P_SetSender || !P_Dispatch ||
        !P_CFRelease || !P_AbsTime)
        return -1;
    uint8_t utf16[MDC_MAX_TEXT * 4 + 8];
    size_t bytes = (size_t)mdc_utf8_to_utf16le(text, utf16, sizeof(utf16));
    if (!bytes) return -1;
    size_t offset = 0;
    while (offset < bytes) {
        size_t chunk = bytes - offset;
        size_t max_chunk = UNICODE_CHUNK_UNITS * 2;
        if (chunk > max_chunk) {
            chunk = max_chunk;
            // never split a surrogate pair across chunks
            uint16_t last_unit = (uint16_t)(utf16[offset + chunk - 2] |
                                            (utf16[offset + chunk - 1] << 8));
            if (last_unit >= 0xD800 && last_unit <= 0xDBFF && bytes - offset > chunk)
                chunk += 2;
        }
        CFRef event = P_CreateUnicode(0, P_AbsTime(), utf16 + offset, (uint32_t)chunk,
                                      1 /* kIOHIDUnicodeEncodingTypeUTF16LE */, 0);
        if (!event) {
            return offset > 0 ? -2 : -1;
        }
        P_SetInt(event, 4 /* built-in flag, same field as the touch path */, 1);
        P_SetSender(event, SENDER);
        P_Dispatch(g_client, event);
        P_CFRelease(event);
        offset += chunk;
        if (offset < bytes) usleep(20000);
    }
    return 0;
}

/* Type a full text string. Unicode HID first (any characters, chunked),
 * per-character ASCII HID as fallback. *out_typed reports characters sent;
 * a failure after events started returns -2 and must not be retried via
 * another path (that would duplicate text). */
static int do_type_text(const char *text, size_t *out_typed) {
    if (out_typed) *out_typed = 0;
    if (!text || !text[0]) return -1;
    int unicode_rc = do_type_text_unicode(text);
    if (unicode_rc == 0) {
        if (out_typed) *out_typed = strlen(text);
        return 0;
    }
    if (unicode_rc == -2) {
        if (out_typed) *out_typed = strlen(text); // partial: do not fall back
        return -2;
    }
    return do_type_text_ascii(text, out_typed);
}

/* Execute a `key` command: [mod+[mod+]]NAME where mods are cmd/shift/alt/ctrl
 * and NAME is a named key (enter, esc, tab, arrows...) or a single character.
 * Presses modifiers, taps the key, releases in reverse. */
static int do_key_command(const char *spec) {
    if (!spec || !spec[0]) return -1;
    if (!g_client || !P_CreateKey || !P_SetSender || !P_Dispatch || !P_CFRelease || !P_AbsTime)
        return -2;
    unsigned short mods[4];
    int mod_count = 0;
    const char *rest = spec;
    int explicit_mods = 0;
    for (;;) {
        const char *plus = strchr(rest, '+');
        if (!plus) break;
        size_t n = (size_t)(plus - rest);
        unsigned short m = 0;
        if (n == 3 && !strncmp(rest, "cmd", 3)) m = 0xE3;
        else if (n == 5 && !strncmp(rest, "shift", 5)) m = 0xE1;
        else if (n == 3 && !strncmp(rest, "alt", 3)) m = 0xE2;
        else if (n == 4 && !strncmp(rest, "ctrl", 4)) m = 0xE0;
        else return -1;
        if (mod_count >= 4) return -1;
        mods[mod_count++] = m;
        explicit_mods = 1;
        rest = plus + 1;
    }
    if (!rest[0]) return -1;
    unsigned short usage = 0;
    int shift = 0;
    if (!rest[1]) {
        if (mdc_key_for_char(rest[0], &usage, &shift) != 0) return -1;
        if (explicit_mods) shift = 0; /* explicit modifiers: use base usage */
    } else {
        static const struct { const char *name; unsigned short usage; } names[] = {
            { "enter", 0x28 }, { "return", 0x28 }, { "esc", 0x29 }, { "escape", 0x29 },
            { "backspace", 0x2A }, { "tab", 0x2B }, { "space", 0x2C },
            { "fdel", 0x4C }, { "delete", 0x4C },
            { "up", 0x52 }, { "down", 0x51 }, { "left", 0x50 }, { "right", 0x4F },
        };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (!strcmp(rest, names[i].name)) { usage = names[i].usage; break; }
        }
        if (!usage) return -1;
    }
    for (int i = 0; i < mod_count; i++) {
        if (keyboard_event_raw(mods[i], 1) != 0) return -1;
        usleep(8000);
    }
    if (shift) { if (keyboard_event_raw(HID_USAGE_SHIFT_LEFT, 1) != 0) return -1; usleep(8000); }
    if (keyboard_event_raw(usage, 1) != 0) return -1;
    usleep(22000);
    if (keyboard_event_raw(usage, 0) != 0) return -1;
    if (shift) { usleep(8000); keyboard_event_raw(HID_USAGE_SHIFT_LEFT, 0); }
    for (int i = mod_count - 1; i >= 0; i--) {
        usleep(8000);
        keyboard_event_raw(mods[i], 0);
    }
    usleep(30000);
    return 0;
}

/* Turn the display on. Prefers the SpringBoard backlight controller; falls
 * back to a hardware menu-key press, which wakes a sleeping display. */
static int do_wake(void) {
    if (tw_rt_init() == 0) {
        OID cls = tw_class("SBBacklightController");
        if (cls) {
            OSEL shared_sel = tw_sel_reg("sharedInstance");
            signed char (*responds)(OID, OSEL, OSEL) = (signed char (*)(OID, OSEL, OSEL))g_tw_msg;
            OID (*send0)(OID, OSEL) = (OID (*)(OID, OSEL))g_tw_msg;
            OID shared = cls && responds(cls, tw_sel_reg("respondsToSelector:"), shared_sel)
                ? send0(cls, shared_sel) : 0;
            if (shared) {
                OSEL from_source = tw_sel_reg("turnOnBacklightFromSource:");
                if (responds(shared, tw_sel_reg("respondsToSelector:"), from_source)) {
                    void (*send1)(OID, OSEL, long) = (void (*)(OID, OSEL, long))g_tw_msg;
                    send1(shared, from_source, 0);
                    return 0;
                }
                OSEL plain = tw_sel_reg("turnOnBacklight");
                if (responds(shared, tw_sel_reg("respondsToSelector:"), plain)) {
                    send0(shared, plain);
                    return 0;
                }
            }
        }
    }
    if (g_hid_ready && do_key(HID_USE_MENU) == 0) return 0;
    return -1;
}

/* --- lock/wake state probing (selector chains ported from witchan/ios-mcp) --- */

static int tw_bool_selector(OID target, const char *name, int *out_value) {
    if (!target || !g_tw_msg) return 0;
    signed char (*responds)(OID, OSEL, OSEL) = (signed char (*)(OID, OSEL, OSEL))g_tw_msg;
    OSEL sel = tw_sel_reg(name);
    if (!responds(target, tw_sel_reg("respondsToSelector:"), sel)) return 0;
    signed char value = ((signed char (*)(OID, OSEL))g_tw_msg)(target, sel);
    if (out_value) *out_value = value ? 1 : 0;
    return 1;
}

static OID tw_shared_instance(const char *class_name) {
    if (tw_rt_init() != 0) return 0;
    OID cls = tw_class(class_name);
    if (!cls) return 0;
    signed char (*responds)(OID, OSEL, OSEL) = (signed char (*)(OID, OSEL, OSEL))g_tw_msg;
    OSEL sel = tw_sel_reg("sharedInstance");
    if (!responds(cls, tw_sel_reg("respondsToSelector:"), sel)) return 0;
    return ((OID (*)(OID, OSEL))g_tw_msg)(cls, sel);
}

/* 1 = locked, 0 = unlocked, -1 = unknown. Probes several selectors across
 * SBLockScreenManager and SpringBoard, then SBLockStateAggregator, then
 * protectedDataAvailable (false implies locked). */
static int sb_lock_state(void) {
    if (tw_rt_init() != 0) return -1;
    int locked = 0, known = 0, value = 0;
    OID lock_manager = tw_shared_instance("SBLockScreenManager");
    OID springboard = tw_shared_instance("SpringBoard");
    static const char *lock_selectors[] = {
        "isUILocked", "isLocked", "isSecurelyLocked", "isDeviceLocked"
    };
    for (size_t i = 0; i < sizeof(lock_selectors) / sizeof(lock_selectors[0]); i++) {
        if (tw_bool_selector(lock_manager, lock_selectors[i], &value) ||
            tw_bool_selector(springboard, lock_selectors[i], &value)) {
            locked = locked || value;
            known = 1;
        }
    }
    if (!known) {
        OID aggregator = tw_shared_instance("SBLockStateAggregator");
        if (aggregator) {
            OSEL sel = tw_sel_reg("lockState");
            signed char (*responds)(OID, OSEL, OSEL) = (signed char (*)(OID, OSEL, OSEL))g_tw_msg;
            if (responds(aggregator, tw_sel_reg("respondsToSelector:"), sel)) {
                long state = ((long (*)(OID, OSEL))g_tw_msg)(aggregator, sel);
                locked = state != 0;
                known = 1;
            }
        }
    }
    if (!known) {
        OID app_class = tw_class("UIApplication");
        signed char (*responds)(OID, OSEL, OSEL) = (signed char (*)(OID, OSEL, OSEL))g_tw_msg;
        OSEL shared = tw_sel_reg("sharedApplication");
        OID app = (app_class && responds(app_class, tw_sel_reg("respondsToSelector:"), shared))
            ? ((OID (*)(OID, OSEL))g_tw_msg)(app_class, shared) : 0;
        if (tw_bool_selector(app, "protectedDataAvailable", &value)) {
            locked = value ? 0 : 1;
            known = 1;
        }
    }
    return known ? locked : -1;
}

static int do_unlock(const char *passcode, char *resp, size_t rsz) {
    do_wake();
    usleep(450000);
    int locked = sb_lock_state();
    if (locked == 0) { snprintf(resp, rsz, "ok unlock already\n"); return 0; }

    if (!g_hid_ready) { snprintf(resp, rsz, "err hid-unavailable\n"); return -1; }
    double sw = 0, sh = 0;
    if (mdc_screen_size(&sw, &sh) != 0) { snprintf(resp, rsz, "err screen-size-unavailable\n"); return -1; }

    double x1 = sw * 0.5, y1 = sh * 0.92;
    double x2 = sw * 0.5, y2 = sh * 0.35;
    double nx1 = 0, ny1 = 0, nx2 = 0, ny2 = 0;
    if (mdc_normalize_hid_point(x1, y1, sw, sh, &nx1, &ny1) != 0) { nx1 = x1 / sw; ny1 = y1 / sh; }
    if (mdc_normalize_hid_point(x2, y2, sw, sh, &nx2, &ny2) != 0) { nx2 = x2 / sw; ny2 = y2 / sh; }
    if (send_phase(0, nx1, ny1) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return -1; }
    usleep(20000);
    for (int i = 1; i <= 14; i++) {
        if (send_phase(1, nx1 + (nx2 - nx1) * i / 14, ny1 + (ny2 - ny1) * i / 14) != 0) {
            snprintf(resp, rsz, "err hid-dispatch-failed\n");
            return -1;
        }
        usleep(15000);
    }
    if (send_phase(2, nx2, ny2) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return -1; }
    usleep(1000000);

    locked = sb_lock_state();
    if (locked == 0) { snprintf(resp, rsz, "ok unlock\n"); return 0; }

    if (locked == 1 && passcode && passcode[0]) {
        for (const char *p = passcode; *p; p++) {
            int digit = *p - '0';
            uint16_t usage = digit == 0 ? 0x27 : (uint16_t)(0x1E + digit - 1);
            if (do_keyboard_key(usage) != 0) break;
        }
        do_keyboard_key(HID_USAGE_ENTER);
        usleep(1000000);
        locked = sb_lock_state();
        if (locked == 0) { snprintf(resp, rsz, "ok unlock\n"); return 0; }
        snprintf(resp, rsz, "err locked\n");
        return -1;
    }
    if (locked == 1) { snprintf(resp, rsz, "err passcode-required\n"); return -1; }
    snprintf(resp, rsz, "ok unlock unverified\n");
    return 0;
}

int ax_dump(char **out, size_t *outlen, int depth); // axdump.c
int ax_insert_text(const char *text);

static void startup_log(const char *stage, int value) {
    char line[128];
    int n = snprintf(line, sizeof(line), "%s %d\n", stage, value);
    if (n <= 0 || (size_t)n >= sizeof(line)) return;
    int fd = open("/var/mobile/Library/Logs/MDControl-startup.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) { write(fd, line, (size_t)n); close(fd); }
}

static void *H_SBS = 0;
static void (*F_SBSLaunch)(const void *, int);
static const void *(*F_CFStrMk)(void *, const char *, unsigned);
static void (*F_CFRelease)(CFRef);

static void ensure_sbs(void) {
    if (H_SBS) return;
    H_SBS = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_LAZY);
    if (!H_SBS) return;
    F_SBSLaunch = dlsym(H_SBS, "SBSLaunchApplicationWithIdentifier");
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
    if (cf) {
        F_CFStrMk = dlsym(cf, "CFStringCreateWithCString");
        F_CFRelease = dlsym(cf, "CFRelease");
    }
}

static int send_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}
static int dispatch_tap(double tx, double ty, double tw, double th, char *resp, size_t rsz) {
    if (!g_hid_ready) { snprintf(resp, rsz, "err hid-unavailable\n"); return -1; }
    int marker_shown = mdc_show_tap(tx, ty, tw, th);
    double x = 0, y = 0;
    if (mdc_normalize_hid_point(tx, ty, tw, th, &x, &y) != 0) {
        x = tx / tw; y = ty / th;
    }
    if (send_phase(0, x, y) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return -1; }
    usleep(60000);
    if (send_phase(2, x, y) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return -1; }
    snprintf(resp, rsz, marker_shown ? "ok tap visible\n" : "ok tap marker-unavailable\n");
    return 0;
}

static void run_command(const MdcCommand *cmd, char *resp, size_t rsz) {
    if (cmd->type == MDC_CMD_PING) { snprintf(resp, rsz, "ok pong\n"); return; }
    if (cmd->type == MDC_CMD_TAP) {
        dispatch_tap(cmd->x1, cmd->y1, cmd->width, cmd->height, resp, rsz);
        return;
    }
    if (cmd->type == MDC_CMD_LONGPRESS) {
        if (!g_hid_ready) { snprintf(resp, rsz, "err hid-unavailable\n"); return; }
        int marker_shown = mdc_show_tap(cmd->x1, cmd->y1, cmd->width, cmd->height);
        double x = 0, y = 0;
        if (mdc_normalize_hid_point(cmd->x1, cmd->y1, cmd->width, cmd->height, &x, &y) != 0) {
            x = cmd->x1 / cmd->width; y = cmd->y1 / cmd->height;
        }
        if (send_phase(0, x, y) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
        usleep((useconds_t)(cmd->hold_ms * 1000));
        if (send_phase(2, x, y) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
        snprintf(resp, rsz, marker_shown ? "ok longpress visible\n" : "ok longpress marker-unavailable\n");
        return;
    }
    if (cmd->type == MDC_CMD_DOUBLETAP) {
        if (!g_hid_ready) { snprintf(resp, rsz, "err hid-unavailable\n"); return; }
        int marker_shown = mdc_show_tap(cmd->x1, cmd->y1, cmd->width, cmd->height);
        double x = 0, y = 0;
        if (mdc_normalize_hid_point(cmd->x1, cmd->y1, cmd->width, cmd->height, &x, &y) != 0) {
            x = cmd->x1 / cmd->width; y = cmd->y1 / cmd->height;
        }
        for (int i = 0; i < 2; i++) {
            if (send_phase(0, x, y) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
            usleep(50000);
            if (send_phase(2, x, y) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
            if (i == 0) usleep(110000);
        }
        snprintf(resp, rsz, marker_shown ? "ok doubletap visible\n" : "ok doubletap marker-unavailable\n");
        return;
    }
    if (cmd->type == MDC_CMD_DRAG) {
        if (!g_hid_ready) { snprintf(resp, rsz, "err hid-unavailable\n"); return; }
        int marker_start = mdc_show_tap(cmd->x1, cmd->y1, cmd->width, cmd->height);
        double nx1 = 0, ny1 = 0, nx2 = 0, ny2 = 0;
        if (mdc_normalize_hid_point(cmd->x1, cmd->y1, cmd->width, cmd->height, &nx1, &ny1) != 0) {
            nx1 = cmd->x1 / cmd->width; ny1 = cmd->y1 / cmd->height;
        }
        if (mdc_normalize_hid_point(cmd->x2, cmd->y2, cmd->width, cmd->height, &nx2, &ny2) != 0) {
            nx2 = cmd->x2 / cmd->width; ny2 = cmd->y2 / cmd->height;
        }
        if (send_phase(0, nx1, ny1) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
        usleep((useconds_t)(cmd->hold_ms * 1000)); /* pick-up hold: no movement */
        for (int i = 1; i <= cmd->steps; i++) {
            if (send_phase(1, nx1 + (nx2 - nx1) * i / cmd->steps,
                           ny1 + (ny2 - ny1) * i / cmd->steps) != 0) {
                snprintf(resp, rsz, "err hid-dispatch-failed\n");
                return;
            }
            usleep(28000);
        }
        usleep(120000); /* settle before release */
        if (send_phase(2, nx2, ny2) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
        int marker_end = mdc_show_tap(cmd->x2, cmd->y2, cmd->width, cmd->height);
        snprintf(resp, rsz,
                 marker_start || marker_end ? "ok drag visible\n" : "ok drag marker-unavailable\n");
        return;
    }
    if (cmd->type == MDC_CMD_KEY) {
        int kr = do_key_command(cmd->text);
        if (kr == -2) { snprintf(resp, rsz, "err key-unavailable\n"); return; }
        if (kr != 0) { snprintf(resp, rsz, "err invalid-key\n"); return; }
        snprintf(resp, rsz, "ok key\n"); return;
    }
    if (cmd->type == MDC_CMD_CLIPBOARD_SET) {
        int cr = mdc_clipboard_set(cmd->text);
        if (cr == -2) { snprintf(resp, rsz, "err clipboard-timeout\n"); return; }
        if (cr != 0) {
            snprintf(resp, rsz, "err clipboard-unavailable\n");
            return;
        }
        snprintf(resp, rsz, "ok clipboard %d\n", (int)strlen(cmd->text));
        return;
    }
    if (cmd->type == MDC_CMD_OPEN_URL) {
        if (mdc_open_url(cmd->text) != 0) { snprintf(resp, rsz, "err open-url-failed\n"); return; }
        snprintf(resp, rsz, "ok open-url\n"); return;
    }
    if (cmd->type == MDC_CMD_SWIPE) {
        if (!g_hid_ready) { snprintf(resp, rsz, "err hid-unavailable\n"); return; }
        int marker_start = mdc_show_tap(cmd->x1, cmd->y1, cmd->width, cmd->height);
        double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (mdc_normalize_hid_point(cmd->x1, cmd->y1, cmd->width, cmd->height, &x1, &y1) != 0) {
            x1 = cmd->x1 / cmd->width; y1 = cmd->y1 / cmd->height;
        }
        if (mdc_normalize_hid_point(cmd->x2, cmd->y2, cmd->width, cmd->height, &x2, &y2) != 0) {
            x2 = cmd->x2 / cmd->width; y2 = cmd->y2 / cmd->height;
        }
        if (send_phase(0, x1, y1) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
        usleep(20000);
        for (int i = 1; i <= cmd->steps; i++) {
            if (send_phase(1, x1 + (x2 - x1) * i / cmd->steps,
                           y1 + (y2 - y1) * i / cmd->steps) != 0) {
                snprintf(resp, rsz, "err hid-dispatch-failed\n"); return;
            }
            usleep(15000);
        }
        if (send_phase(2, x2, y2) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
        int marker_end = mdc_show_tap(cmd->x2, cmd->y2, cmd->width, cmd->height);
        snprintf(resp, rsz, marker_start || marker_end ? "ok swipe visible\n" : "ok swipe marker-unavailable\n"); return;
    }
    if (cmd->type == MDC_CMD_HOME || cmd->type == MDC_CMD_VOLUP) {
        if (!g_hid_ready) { snprintf(resp, rsz, "err hid-unavailable\n"); return; }
        uint16_t usage = cmd->type == MDC_CMD_HOME ? HID_USE_MENU : HID_USE_VOLUP;
        if (do_key(usage) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
        snprintf(resp, rsz, "ok %s\n", cmd->type == MDC_CMD_HOME ? "home" : "volup"); return;
    }
    if (cmd->type == MDC_CMD_LAUNCH) {
        ensure_sbs();
        if (!F_SBSLaunch || !F_CFStrMk || !F_CFRelease) {
            snprintf(resp, rsz, "err launch-unavailable\n"); return;
        }
        const void *bundle = F_CFStrMk(0, cmd->bundle_id, 0x08000100u);
        if (!bundle) { snprintf(resp, rsz, "err launch-failed\n"); return; }
        F_SBSLaunch(bundle, 0);
        F_CFRelease((CFRef)bundle);
        snprintf(resp, rsz, "ok launch\n"); return;
    }
    if (cmd->type == MDC_CMD_TYPE) {
        if (g_hid_ready) {
            size_t typed = 0;
            int tr = do_type_text(cmd->text, &typed);
            if (tr == 0) { snprintf(resp, rsz, "ok type %d\n", (int)typed); return; }
            if (typed > 0) {
                /* partial HID run: do not double-insert via the AX fallback */
                snprintf(resp, rsz, "err type-interrupted %d\n", (int)typed);
                return;
            }
        }
        if (ax_insert_text(cmd->text) == 0) { snprintf(resp, rsz, "ok type\n"); return; }
        snprintf(resp, rsz, "err text-unavailable\n"); return;
    }
    if (cmd->type == MDC_CMD_NOTIFY) {
        if (mdc_notify(cmd->text) != 0) { snprintf(resp, rsz, "err notify-unavailable\n"); return; }
        snprintf(resp, rsz, "ok notify\n"); return;
    }
    if (cmd->type == MDC_CMD_TOAST) {
        if (mdc_toast(cmd->text) == 0) { snprintf(resp, rsz, "err toast-unavailable\n"); return; }
        snprintf(resp, rsz, "ok toast\n"); return;
    }
    if (cmd->type == MDC_CMD_SAY) {
        int rc = mdc_say(cmd->text, cmd->amount, cmd->voice, cmd->interruptible);
        if (rc == 0) { snprintf(resp, rsz, "ok say\n"); return; }
        if (rc == -3) { snprintf(resp, rsz, "ok say interrupted\n"); return; }
        if (rc == -2) { snprintf(resp, rsz, "ok say timeout\n"); return; }
        snprintf(resp, rsz, "err say-unavailable\n"); return;
    }
    if (cmd->type == MDC_CMD_LISTEN_TURN) {
        char path[64];
        snprintf(path, sizeof(path), "%s", "/var/mobile/Downloads/psychosis-turn.wav");
        double seconds = 0;
        int rc = mdc_listen_turn(cmd->timeout, cmd->silence_ms, cmd->min_speech_ms,
                                 path, sizeof(path), &seconds);
        if (rc == 0) { snprintf(resp, rsz, "ok turn %s %.1f\n", path, seconds); return; }
        if (rc == -4) { snprintf(resp, rsz, "err no-speech\n"); return; }
        snprintf(resp, rsz, "err listen-unavailable\n"); return;
    }
    if (cmd->type == MDC_CMD_RECORD) {
        char path[MDC_MAX_PATH + 1];
        memset(path, 0, sizeof(path));
        if (cmd->path[0]) snprintf(path, sizeof(path), "%s", cmd->path);
        int rr = mdc_record(cmd->amount, path, sizeof(path));
        if (rr == 0) { snprintf(resp, rsz, "ok record %s\n", path); return; }
        if (rr == -3) { snprintf(resp, rsz, "err mic-unavailable\n"); return; }
        snprintf(resp, rsz, "err record-unavailable\n"); return;
    }
    if (cmd->type == MDC_CMD_SHOT) {
        char path[MDC_MAX_PATH + 1];
        memset(path, 0, sizeof(path));
        if (cmd->path[0]) snprintf(path, sizeof(path), "%s", cmd->path);
        int shot_w = 0, shot_h = 0;
        int rc = mdc_shot(path, sizeof(path), &shot_w, &shot_h);
        if (rc != 0) { snprintf(resp, rsz, "err shot-unavailable\n"); return; }
        snprintf(resp, rsz, "ok shot %s %dx%d\n", path, shot_w, shot_h); return;
    }
    if (cmd->type == MDC_CMD_TAPNAME) {
        double tx = 0, ty = 0;
        int found = ax_find_center(cmd->text, &tx, &ty);
        if (found == -3) { snprintf(resp, rsz, "err ax-busy\n"); return; }
        if (found != 0) { snprintf(resp, rsz, "err not-found\n"); return; }
        if (!g_hid_ready) { snprintf(resp, rsz, "err hid-unavailable\n"); return; }
        /* AX frames are fixed-coordinate-space points; HID wants normalized
         * fixed-space coordinates, and the marker wants current-space. */
        double fx = 0, fy = 0;
        if (mdc_fixed_screen_size(&fx, &fy) != 0 || fx <= 0 || fy <= 0) {
            snprintf(resp, rsz, "err screen-size-unavailable\n"); return;
        }
        double nx = tx / fx, ny = ty / fy;
        if (nx < 0) nx = 0;
        if (nx > 1) nx = 1;
        if (ny < 0) ny = 0;
        if (ny > 1) ny = 1;
        int marker_shown = 0;
        double cx = tx, cy = ty, cw = 0, ch = 0;
        if (mdc_fixed_to_current(tx, ty, &cx, &cy) == 0 && mdc_screen_size(&cw, &ch) == 0)
            marker_shown = mdc_show_tap(cx, cy, cw, ch);
        if (send_phase(0, nx, ny) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
        usleep(60000);
        if (send_phase(2, nx, ny) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
        snprintf(resp, rsz, marker_shown ? "ok tap visible\n" : "ok tap marker-unavailable\n");
        return;
    }
    if (cmd->type == MDC_CMD_WAIT) {
        int gone = 0;
        const char *label = cmd->text;
        if (!strncmp(label, "--gone ", 7)) {
            gone = 1;
            label += 7;
            while (*label == ' ') label++;
        }
        if (!label[0]) { snprintf(resp, rsz, "err invalid-arguments\n"); return; }
        for (int attempt = 0; attempt < 24; attempt++) {
            double tx = 0, ty = 0;
            int found = ax_find_center(label, &tx, &ty);
            if (gone) {
                if (found == -2) { snprintf(resp, rsz, "ok gone\n"); return; }
                if (found == -1) { snprintf(resp, rsz, "err ax-unavailable\n"); return; }
            } else if (found == 0) {
                double cx = tx, cy = ty, cw = 0, ch = 0;
                if (mdc_fixed_to_current(tx, ty, &cx, &cy) == 0 &&
                    mdc_screen_size(&cw, &ch) == 0)
                    snprintf(resp, rsz, "ok found %d %d %d %d\n",
                             (int)cx, (int)cy, (int)cw, (int)ch);
                else
                    snprintf(resp, rsz, "ok found %d %d\n", (int)tx, (int)ty);
                return;
            } else if (found == -1) {
                snprintf(resp, rsz, "err ax-unavailable\n");
                return;
            }
            usleep(450000);
        }
        snprintf(resp, rsz, "err timeout\n"); return;
    }
    if (cmd->type == MDC_CMD_FRONT) {
        char bundle[256];
        int pid = 0, system_root = 0;
        memset(bundle, 0, sizeof(bundle));
        if (ax_front_info(bundle, sizeof(bundle), &pid, &system_root) != 0) {
            snprintf(resp, rsz, "err no-front-app\n"); return;
        }
        snprintf(resp, rsz, "ok %s %d%s\n", bundle, pid, system_root ? " home" : ""); return;
    }
    if (cmd->type == MDC_CMD_WAKE) {
        if (do_wake() != 0) { snprintf(resp, rsz, "err wake-unavailable\n"); return; }
        snprintf(resp, rsz, "ok wake\n"); return;
    }
    if (cmd->type == MDC_CMD_UNLOCK) {
        do_unlock(cmd->text, resp, rsz);
        return;
    }
    if (cmd->type == MDC_CMD_SCROLL) {
        if (!g_hid_ready) { snprintf(resp, rsz, "err hid-unavailable\n"); return; }
        double sw = cmd->width, sh = cmd->height;
        double amount = cmd->amount;
        /* default: 60% of the axis the gesture travels along */
        if (amount <= 0) amount = (cmd->direction <= 1 ? sh : sw) * 0.6;
        double max_along = (cmd->direction <= 1 ? sh : sw) * 0.84;
        if (amount > max_along) amount = max_along;
        double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        /* finger travels opposite the content direction:
         * scroll down = finger up, scroll up = finger down, etc. */
        if (cmd->direction == 0) {        /* up: content moves down */
            x1 = sw / 2; y1 = sh * 0.28; x2 = sw / 2; y2 = y1 + amount;
        } else if (cmd->direction == 1) { /* down: content moves up */
            x1 = sw / 2; y1 = sh * 0.72; x2 = sw / 2; y2 = y1 - amount;
        } else if (cmd->direction == 2) { /* left: content moves right */
            x1 = sw * 0.28; y1 = sh / 2; x2 = x1 + amount; y2 = sh / 2;
        } else {                          /* right: content moves left */
            x1 = sw * 0.72; y1 = sh / 2; x2 = x1 - amount; y2 = sh / 2;
        }
        if (y2 < sh * 0.08) y2 = sh * 0.08;
        if (y2 > sh * 0.92) y2 = sh * 0.92;
        if (x2 < sw * 0.08) x2 = sw * 0.08;
        if (x2 > sw * 0.92) x2 = sw * 0.92;

        int marker_start = mdc_show_tap(x1, y1, sw, sh);
        double nx1 = 0, ny1 = 0, nx2 = 0, ny2 = 0;
        if (mdc_normalize_hid_point(x1, y1, sw, sh, &nx1, &ny1) != 0) { nx1 = x1 / sw; ny1 = y1 / sh; }
        if (mdc_normalize_hid_point(x2, y2, sw, sh, &nx2, &ny2) != 0) { nx2 = x2 / sw; ny2 = y2 / sh; }
        double distance = cmd->direction <= 1 ? (y2 - y1) : (x2 - x1);
        if (distance < 0) distance = -distance;
        int steps = (int)(distance / 25.0);
        if (steps < 12) steps = 12;
        if (steps > 40) steps = 40;
        if (send_phase(0, nx1, ny1) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
        usleep(25000);
        for (int i = 1; i <= steps; i++) {
            if (send_phase(1, nx1 + (nx2 - nx1) * i / steps, ny1 + (ny2 - ny1) * i / steps) != 0) {
                snprintf(resp, rsz, "err hid-dispatch-failed\n");
                return;
            }
            usleep(22000);
        }
        /* press-and-hold before liftoff kills inertial momentum -> precise stop */
        usleep(90000);
        if (send_phase(2, nx2, ny2) != 0) { snprintf(resp, rsz, "err hid-dispatch-failed\n"); return; }
        int marker_end = mdc_show_tap(x2, y2, sw, sh);
        snprintf(resp, rsz, marker_start || marker_end ? "ok scroll %d\n" : "ok scroll %d marker-unavailable\n", steps);
        return;
    }
    snprintf(resp, rsz, "err invalid-command\n");
}

// Read one bounded, newline-terminated request. TCP packet boundaries are not
// protocol boundaries, so fragmented and coalesced writes are handled here.
static int read_line(int fd, char *line, size_t cap) {
    size_t used = 0;
    while (used + 1 < cap) {
        char chunk[128];
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return used ? -2 : -1;
        for (ssize_t i = 0; i < n; i++) {
            if (chunk[i] == '\n') { line[used] = 0; return 0; }
            if (used + 1 >= cap) return -3;
            line[used++] = chunk[i];
        }
    }
    return -3;
}

static void *server_thread(void *arg) {
    (void)arg;
    g_hid_ready = (hid_init() == 0);
    startup_log("hid", g_hid_ready);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { startup_log("socket", errno); return 0; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(6001);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) { startup_log("bind", errno); close(s); return 0; }
    if (listen(s, 4) != 0) { startup_log("listen", errno); close(s); return 0; }
    startup_log("listening", 6001);
    for (;;) {
        int c = accept(s, 0, 0);
        if (c < 0) continue;
        char line[MDC_MAX_LINE + 2];
        struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
        int no_sigpipe = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
        int read_rc = read_line(c, line, sizeof(line));
        if (read_rc != 0) {
            const char *e = read_rc == -3 ? "err line-too-long\n" : "err incomplete-command\n";
            send_all(c, e, strlen(e));
            close(c);
            continue;
        }
        MdcCommand cmd;
        char error[64];
        if (mdc_parse_line(line, &cmd, error, sizeof(error)) != 0) {
            char resp[96];
            snprintf(resp, sizeof(resp), "err %s\n", error);
            send_all(c, resp, strlen(resp));
        } else if (cmd.type == MDC_CMD_AX) {
            char *js = 0; size_t jl = 0;
            int r = ax_dump(&js, &jl, cmd.depth);
            if (r == 0 && js && jl) {
                send_all(c, js, jl);
                send_all(c, "\n", 1);
                free(js);
            } else if (r == -2) {
                const char *e = "{\"error\":\"ax-empty\"}\n";
                send_all(c, e, strlen(e));
            } else if (r == -3) {
                const char *e = "{\"error\":\"ax-busy\"}\n";
                send_all(c, e, strlen(e));
            } else if (r == -4) {
                const char *e = "{\"error\":\"ax-timeout\"}\n";
                send_all(c, e, strlen(e));
            } else {
                const char *e = "{\"error\":\"ax-unavailable\"}\n";
                send_all(c, e, strlen(e));
            }
        } else if (cmd.type == MDC_CMD_CLIPBOARD_GET) {
            char *buf = malloc(131072);
            int cr = buf ? mdc_clipboard_get(buf, 131072) : -1;
            if (cr == 0 && buf) {
                send_all(c, buf, strlen(buf));
                send_all(c, "\n", 1);
            } else if (cr == -2) {
                const char *e = "err clipboard-timeout\n";
                send_all(c, e, strlen(e));
            } else {
                const char *e = "err clipboard-unavailable\n";
                send_all(c, e, strlen(e));
            }
            free(buf);
        } else if (cmd.type == MDC_CMD_SAY && !strcmp(cmd.text, "voices")) {
            char *list = malloc(8192);
            int vr = list ? mdc_say_voices(list, 8192) : -1;
            if (vr == 0 && list && list[0]) {
                send_all(c, list, strlen(list));
            } else {
                const char *e = "err voices-unavailable\n";
                send_all(c, e, strlen(e));
            }
            free(list);
        } else {
            char resp[128];
            run_command(&cmd, resp, sizeof(resp));
            send_all(c, resp, strlen(resp));
        }
        close(c);
    }
    return 0;
}

__attribute__((constructor)) static void mdcontrol_init(void) {
    startup_log("constructor", 1);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &a, server_thread, 0);
    startup_log("pthread_create", rc);
    pthread_attr_destroy(&a);
}
