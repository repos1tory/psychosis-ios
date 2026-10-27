// Bounded Accessibility tree export from SpringBoard's frontmost app.
// UI identity is read on the main queue; all remote AX queries run on the
// server worker thread with per-element and whole-query limits.
#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "mdc_protocol.h"
#include "mdc_mainthread.h"

typedef void *OID;
typedef void *OSEL;
typedef struct { double x, y, w, h; } R64;
typedef void *AXRef;
typedef int AXError;
typedef AXRef (*AXCreateApplicationFn)(int);
typedef AXRef (*AXCreateSystemWideFn)(void);
typedef AXError (*AXCopyAttributeValueFn)(AXRef, AXRef, AXRef *);
typedef AXError (*AXCopyAttributeNamesFn)(AXRef, AXRef *);
typedef AXError (*AXGetPidFn)(AXRef, int *);
typedef AXError (*AXSetTimeoutFn)(AXRef, float);
typedef void (*AXAddAssociatedPidFn)(int, int, int);
typedef bool (*AXIsPidAssociatedFn)(int);
typedef bool (*AXIsPidAssociatedWithDisplayTypeFn)(int, int);
typedef void (*AXSetRequestingClientFn)(uint32_t);
typedef uint64_t (*AXOverrideRequestingClientTypeFn)(uint64_t);
typedef bool (*AXBooleanReadFn)(void);
typedef void (*AXBooleanWriteFn)(bool);
typedef void (*AXNoArgumentFn)(void);
typedef void (*CFReleaseFn)(AXRef);
typedef AXRef (*CopyFrontmostDisplayIdFn)(void);
typedef int (*ProcessIdForDisplayIdFn)(AXRef, uint32_t *);

#define MAXNODES 160
#define MAXDEPTH MDC_MAX_AX_DEPTH
#define AX_TIMEOUT_SECONDS 0.20f
#define QUERY_BUDGET_NS 4000000000ull

static void *H_AX = 0, *H_OBJC = 0, *H_CF = 0, *H_AXUI = 0, *H_AXUTIL = 0;
static void *H_AX_HANDLES[12];
static size_t H_AX_HANDLE_COUNT;
static int G_AX_RUNTIME_INDEX = -2;
static OID (*F_getClass)(const char *);
static OSEL (*F_selReg)(const char *);
static OID (*M_id0)(OID, OSEL);
static OID (*M_id1)(OID, OSEL, OID);
static void (*M_void1)(OID, OSEL, OID);
static OID (*M_at)(OID, OSEL, unsigned long);
static OID (*M_numi)(OID, OSEL, int);
static OID (*M_cstr_new)(OID, OSEL, const char *);
static const char *(*M_cstr)(OID, OSEL);
static unsigned long (*M_count)(OID, OSEL);
static R64 (*M_rect)(OID, OSEL);
static int (*M_int)(OID, OSEL);
static bool (*M_bool1)(OID, OSEL, OID);
static OID (*M_json)(OID, OSEL, OID, unsigned long, void **);
static const void *(*M_bytes)(OID, OSEL);
static void (*M_void0)(OID, OSEL);
static AXCreateApplicationFn F_createApplication;
static AXCreateSystemWideFn F_createSystemWide;
static AXCopyAttributeValueFn F_copyAttributeValue;
static AXCopyAttributeNamesFn F_copyAttributeNames;
static AXGetPidFn F_getPid;
static AXSetTimeoutFn F_setMessagingTimeout;
static AXAddAssociatedPidFn F_addAssociatedPid;
static AXIsPidAssociatedFn F_isPidAssociated;
static AXIsPidAssociatedWithDisplayTypeFn F_isPidAssociatedWithDisplayType;
static AXSetRequestingClientFn F_setRequestingClient;
static AXOverrideRequestingClientTypeFn F_overrideRequestingClientType;
static AXBooleanReadFn F_applicationAccessibilityEnabled;
static AXBooleanWriteFn F_setApplicationAccessibilityEnabled;
static AXBooleanReadFn F_voiceOverTouchUsageConfirmed;
static AXBooleanWriteFn F_setVoiceOverTouchUsageConfirmed;
static AXBooleanReadFn F_voiceOverTouchEnabled;
static AXNoArgumentFn F_primeAccessibilityDisplayManager;
static CFReleaseFn F_CFRelease;
static CopyFrontmostDisplayIdFn F_copyFrontmostDisplayId;
static ProcessIdForDisplayIdFn F_processIdForDisplayId;

static int g_axui_bootstrap_attempted;
static int g_axui_framework_loaded;
static int g_axui_display_manager_primed;
static int g_axui_application_accessibility_before = -1;
static int g_axui_application_accessibility_after = -1;
static int g_axui_voiceover_usage_before = -1;
static int g_axui_voiceover_usage_after = -1;
static int g_axui_voiceover_touch_before = -1;
static int g_axui_voiceover_touch_after = -1;
static int g_axui_client_created;
static int g_axui_delegate_created;
static int g_axui_delegate_set;
static int g_axui_register_sent;
static int g_axui_caption_sent;
static int g_axui_curtain_disabled_sent;
static volatile int g_axui_server_ready;
static char g_axui_bootstrap_error[160];
static OID g_axui_client;
static OID g_axui_delegate;
static void *(*F_objc_allocate_class_pair)(void *, const char *, unsigned long);
static void (*F_objc_register_class_pair)(void *);
static bool (*F_class_add_method)(void *, OSEL, void (*)(void), const char *);
static void (*F_axui_send_message)(OID, OSEL, OID, unsigned long, OID, OID);
static OID (*M_obj2)(OID, OSEL, OID, OID);

static void load_ax_handles(void) {
    static const char *paths[] = {
        "/System/Library/Frameworks/ApplicationServices.framework/Frameworks/HIServices.framework/HIServices",
        "/System/Library/PrivateFrameworks/AXRuntime.framework/AXRuntime",
        "/System/Library/Frameworks/Accessibility.framework/Accessibility",
        "/System/Library/PrivateFrameworks/Accessibility.framework/Accessibility",
        "/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices",
        "/System/Library/Frameworks/HIServices.framework/HIServices",
        "/usr/lib/libAccessibility.dylib"
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && H_AX_HANDLE_COUNT < 12; i++) {
        void *handle = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
        if (handle) H_AX_HANDLES[H_AX_HANDLE_COUNT++] = handle;
    }
}

static void *ax_symbol(const char *name) {
    void *symbol = dlsym(RTLD_DEFAULT, name);
    if (symbol) return symbol;
    for (size_t i = 0; i < H_AX_HANDLE_COUNT; i++) {
        symbol = dlsym(H_AX_HANDLES[i], name);
        if (symbol) return symbol;
    }
    if (H_AXUTIL && (symbol = dlsym(H_AXUTIL, name))) return symbol;
    if (H_AXUI && (symbol = dlsym(H_AXUI, name))) return symbol;
    return H_AX ? dlsym(H_AX, name) : 0;
}

static void bind_ax_runtime(void) {
    void *runtime = 0;
    for (size_t i = 0; i < H_AX_HANDLE_COUNT; i++) {
        AXCreateApplicationFn create = (AXCreateApplicationFn)dlsym(H_AX_HANDLES[i], "AXUIElementCreateApplication");
        if (!create) create = (AXCreateApplicationFn)dlsym(H_AX_HANDLES[i], "_AXUIElementCreateAppElementWithPid");
        AXCopyAttributeValueFn copy = (AXCopyAttributeValueFn)dlsym(H_AX_HANDLES[i], "AXUIElementCopyAttributeValue");
        if (!create || !copy) continue;
        runtime = H_AX_HANDLES[i];
        G_AX_RUNTIME_INDEX = (int)i;
        F_createApplication = create;
        F_copyAttributeValue = copy;
        break;
    }
    // RTLD_DEFAULT is a compatibility fallback for runtimes that do not expose
    // one image handle with both core functions.
    if (!runtime) {
        F_createApplication = ax_symbol("AXUIElementCreateApplication");
        if (!F_createApplication) F_createApplication = ax_symbol("_AXUIElementCreateAppElementWithPid");
        F_copyAttributeValue = ax_symbol("AXUIElementCopyAttributeValue");
        if (F_createApplication && F_copyAttributeValue) G_AX_RUNTIME_INDEX = -1;
    }
    if (runtime) {
        F_createSystemWide = (AXCreateSystemWideFn)dlsym(runtime, "AXUIElementCreateSystemWide");
        F_copyAttributeNames = (AXCopyAttributeNamesFn)dlsym(runtime, "AXUIElementCopyAttributeNames");
        F_getPid = (AXGetPidFn)dlsym(runtime, "AXUIElementGetPid");
        F_setMessagingTimeout = (AXSetTimeoutFn)dlsym(runtime, "AXUIElementSetMessagingTimeout");
        F_addAssociatedPid = (AXAddAssociatedPidFn)dlsym(runtime, "_AXAddAssociatedPid");
        F_isPidAssociated = (AXIsPidAssociatedFn)dlsym(runtime, "_AXIsPidAssociated");
        F_isPidAssociatedWithDisplayType = (AXIsPidAssociatedWithDisplayTypeFn)dlsym(runtime, "_AXIsPidAssociatedWithDisplayType");
        F_setRequestingClient = (AXSetRequestingClientFn)dlsym(runtime, "__AXSetRequestingClient");
        F_overrideRequestingClientType = (AXOverrideRequestingClientTypeFn)dlsym(runtime, "_AXOverrideRequestingClientType");
    }
    if (!F_createSystemWide) F_createSystemWide = ax_symbol("AXUIElementCreateSystemWide");
    if (!F_copyAttributeNames) F_copyAttributeNames = ax_symbol("AXUIElementCopyAttributeNames");
    if (!F_getPid) F_getPid = ax_symbol("AXUIElementGetPid");
    if (!F_setMessagingTimeout) F_setMessagingTimeout = ax_symbol("AXUIElementSetMessagingTimeout");
    if (!F_addAssociatedPid) F_addAssociatedPid = ax_symbol("_AXAddAssociatedPid");
    if (!F_isPidAssociated) F_isPidAssociated = ax_symbol("_AXIsPidAssociated");
    if (!F_isPidAssociatedWithDisplayType) F_isPidAssociatedWithDisplayType = ax_symbol("_AXIsPidAssociatedWithDisplayType");
    if (!F_setRequestingClient) F_setRequestingClient = ax_symbol("__AXSetRequestingClient");
    if (!F_overrideRequestingClientType) F_overrideRequestingClientType = ax_symbol("_AXOverrideRequestingClientType");
    F_applicationAccessibilityEnabled = ax_symbol("_AXSApplicationAccessibilityEnabled");
    if (!F_applicationAccessibilityEnabled) F_applicationAccessibilityEnabled = ax_symbol("__AXSApplicationAccessibilityEnabled");
    F_setApplicationAccessibilityEnabled = ax_symbol("_AXSApplicationAccessibilitySetEnabled");
    if (!F_setApplicationAccessibilityEnabled) F_setApplicationAccessibilityEnabled = ax_symbol("__AXSApplicationAccessibilitySetEnabled");
    F_voiceOverTouchUsageConfirmed = ax_symbol("_AXSVoiceOverTouchUsageConfirmed");
    if (!F_voiceOverTouchUsageConfirmed) F_voiceOverTouchUsageConfirmed = ax_symbol("__AXSVoiceOverTouchUsageConfirmed");
    F_setVoiceOverTouchUsageConfirmed = ax_symbol("_AXSVoiceOverTouchSetUsageConfirmed");
    if (!F_setVoiceOverTouchUsageConfirmed) F_setVoiceOverTouchUsageConfirmed = ax_symbol("__AXSVoiceOverTouchSetUsageConfirmed");
    F_voiceOverTouchEnabled = ax_symbol("_AXSVoiceOverTouchEnabled");
    if (!F_voiceOverTouchEnabled) F_voiceOverTouchEnabled = ax_symbol("__AXSVoiceOverTouchEnabled");
    F_primeAccessibilityDisplayManager = ax_symbol("_AXDevicePrimeDisplayManager");
}

static OSEL S(const char *n) { return F_selReg(n); }
static OID C(const char *n) { return F_getClass(n); }
static int g_nodes = 0, inited = 0;
static int g_max_depth = MAXDEPTH;
static uint64_t g_deadline_ns;
static int g_ax_query_route_logged;
static int g_ax_attribute_log_count;
static pthread_mutex_t g_ax_busy_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_ax_busy;

static void ax_log(const char *fmt, ...) {
    char line[320];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);
    if (n <= 0) return;
    size_t len = (size_t)n < sizeof(line) - 2 ? (size_t)n : sizeof(line) - 2;
    line[len++] = '\n';
    int fd = open("/var/mobile/Library/Logs/Psychosis-ax.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) { (void)write(fd, line, len); close(fd); }
}

static void ax_log_reset(void) {
    int fd = open("/var/mobile/Library/Logs/Psychosis-ax.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) close(fd);
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool query_deadline_reached(void) {
    return g_deadline_ns && monotonic_ns() >= g_deadline_ns;
}

static int ax_init(void) {
    if (inited) return 0;
    H_OBJC = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY);
    H_AX = dlopen("/System/Library/PrivateFrameworks/AXRuntime.framework/AXRuntime", RTLD_LAZY | RTLD_GLOBAL);
    H_CF = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
    load_ax_handles();
    void *H_SBS = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_LAZY);
    if (!H_OBJC || !H_AX || !H_CF) return -1;
    F_getClass = dlsym(H_OBJC, "objc_getClass");
    F_selReg = dlsym(H_OBJC, "sel_registerName");
    void *ms = dlsym(H_OBJC, "objc_msgSend");
    M_id0 = ms; M_id1 = ms; M_at = ms; M_numi = ms; M_void1 = ms;
    M_cstr_new = ms; M_cstr = ms; M_count = ms; M_rect = ms;
    M_int = ms; M_bool1 = ms; M_json = ms; M_bytes = ms; M_void0 = ms;
    M_obj2 = ms;
    F_objc_allocate_class_pair = dlsym(H_OBJC, "objc_allocateClassPair");
    F_objc_register_class_pair = dlsym(H_OBJC, "objc_registerClassPair");
    F_class_add_method = dlsym(H_OBJC, "class_addMethod");
    F_axui_send_message = ms;
    bind_ax_runtime();
    F_CFRelease = dlsym(H_CF, "CFRelease");
    if (H_SBS) {
        F_copyFrontmostDisplayId = dlsym(H_SBS, "SBSCopyFrontmostApplicationDisplayIdentifier");
        F_processIdForDisplayId = dlsym(H_SBS, "SBSProcessIDForDisplayIdentifier");
    }
    if (!F_getClass || !F_selReg || !ms || !F_createApplication ||
        !F_setMessagingTimeout || !F_CFRelease) return -1;
    inited = 1;
    return 0;
}

static OID NS(const char *s) {
    return M_cstr_new(M_id0(C("NSString"), S("alloc")), S("initWithUTF8String:"), s);
}

static OID begin_pool(void) {
    return M_id0(M_id0(C("NSAutoreleasePool"), S("alloc")), S("init"));
}

static void dset(OID dict, const char *key, OID obj);
static void setBool(OID dict, const char *key, bool value);

static void axui_delegate_will_activate(OID self, OSEL command, OID client, OID message) {
    (void)self; (void)command; (void)client; (void)message;
    g_axui_server_ready = 1;
    ax_log("AXUIClient server activated");
}

static OID axui_delegate_process_message(OID self, OSEL command, OID client,
                                         OID message, OID identifier, OID *error) {
    (void)self; (void)command; (void)client; (void)message; (void)identifier;
    if (error) *error = 0;
    return 0;
}

static void axui_send_dictionary(OID client, const char *selector_name,
                                 OID message, unsigned long identifier) {
    OSEL selector = S(selector_name);
    if (!client || !message || !M_bool1(client, S("respondsToSelector:"), (OID)selector)) return;
    if (!F_axui_send_message) return;
    F_axui_send_message(client, selector, message, identifier, 0, 0);
}

static void axui_bootstrap_main(void *context) {
    (void)context;
    g_axui_bootstrap_attempted = 1;
    if (!H_AXUI)
        H_AXUI = dlopen("/System/Library/PrivateFrameworks/AccessibilityUI.framework/AccessibilityUI",
                        RTLD_NOW | RTLD_GLOBAL);
    if (!H_AXUTIL)
        H_AXUTIL = dlopen("/System/Library/PrivateFrameworks/AccessibilityUtilities.framework/AccessibilityUtilities",
                          RTLD_NOW | RTLD_GLOBAL);
    g_axui_framework_loaded = C("AXUIClient") != 0;
    bind_ax_runtime();

    if (F_applicationAccessibilityEnabled)
        g_axui_application_accessibility_before = F_applicationAccessibilityEnabled() ? 1 : 0;
    if (F_voiceOverTouchUsageConfirmed)
        g_axui_voiceover_usage_before = F_voiceOverTouchUsageConfirmed() ? 1 : 0;
    if (F_voiceOverTouchEnabled)
        g_axui_voiceover_touch_before = F_voiceOverTouchEnabled() ? 1 : 0;
    if (F_primeAccessibilityDisplayManager) {
        F_primeAccessibilityDisplayManager();
        g_axui_display_manager_primed = 1;
    }
    if (F_setApplicationAccessibilityEnabled &&
        (g_axui_application_accessibility_before < 0 || !g_axui_application_accessibility_before))
        F_setApplicationAccessibilityEnabled(true);
    if (F_setVoiceOverTouchUsageConfirmed &&
        (g_axui_voiceover_usage_before < 0 || !g_axui_voiceover_usage_before))
        F_setVoiceOverTouchUsageConfirmed(true);
    if (F_applicationAccessibilityEnabled)
        g_axui_application_accessibility_after = F_applicationAccessibilityEnabled() ? 1 : 0;
    if (F_voiceOverTouchUsageConfirmed)
        g_axui_voiceover_usage_after = F_voiceOverTouchUsageConfirmed() ? 1 : 0;
    if (F_voiceOverTouchEnabled)
        g_axui_voiceover_touch_after = F_voiceOverTouchEnabled() ? 1 : 0;

    if (g_axui_register_sent && g_axui_client) return;
    if (!F_objc_allocate_class_pair || !F_objc_register_class_pair || !F_class_add_method) {
        snprintf(g_axui_bootstrap_error, sizeof(g_axui_bootstrap_error), "Objective-C runtime class APIs unavailable");
        return;
    }
    OID client_class = C("AXUIClient");
    if (!client_class) {
        snprintf(g_axui_bootstrap_error, sizeof(g_axui_bootstrap_error), "AXUIClient class unavailable");
        return;
    }
    void *delegate_class = C("MDCAxUIClientDelegate");
    if (!delegate_class) {
        delegate_class = F_objc_allocate_class_pair(C("NSObject"), "MDCAxUIClientDelegate", 0);
        if (!delegate_class) {
            snprintf(g_axui_bootstrap_error, sizeof(g_axui_bootstrap_error), "delegate class allocation failed");
            return;
        }
        bool activate_added = F_class_add_method(delegate_class,
            S("userInterfaceClient:willActivateUserInterfaceServiceWithInitializationMessage:"),
            (void (*)(void))axui_delegate_will_activate, "v@:@@");
        bool process_added = F_class_add_method(delegate_class,
            S("userInterfaceClient:processMessageFromServer:withIdentifier:error:"),
            (void (*)(void))axui_delegate_process_message, "@@:@@@^@");
        if (!activate_added || !process_added) {
            snprintf(g_axui_bootstrap_error, sizeof(g_axui_bootstrap_error), "delegate methods could not be registered");
            return;
        }
        F_objc_register_class_pair(delegate_class);
        g_axui_delegate_created = 1;
    }
    if (!g_axui_delegate) {
        OID allocated = M_id0((OID)delegate_class, S("alloc"));
        g_axui_delegate = allocated ? M_id0(allocated, S("init")) : 0;
    }
    if (!g_axui_delegate) {
        snprintf(g_axui_bootstrap_error, sizeof(g_axui_bootstrap_error), "delegate instance creation failed");
        return;
    }
    g_axui_delegate_created = 1;

    if (!g_axui_client) {
        OID identifier = NS("VOTAXUIClientIdentifier");
        OID service_name = NS("VoiceOver");
        OID allocated = M_id0(client_class, S("alloc"));
        g_axui_client = allocated
            ? M_obj2(allocated, S("initWithIdentifier:serviceBundleName:"), identifier, service_name)
            : 0;
        if (identifier) M_void0(identifier, S("release"));
        if (service_name) M_void0(service_name, S("release"));
    }
    g_axui_client_created = g_axui_client != 0;
    if (!g_axui_client) {
        snprintf(g_axui_bootstrap_error, sizeof(g_axui_bootstrap_error), "AXUIClient initialization failed");
        return;
    }
    OSEL set_delegate_selector = S("setDelegate:");
    if (M_bool1(g_axui_client, S("respondsToSelector:"), (OID)set_delegate_selector)) {
        M_void1(g_axui_client, set_delegate_selector, g_axui_delegate);
        g_axui_delegate_set = 1;
    }

    OID registration = M_id0(C("NSMutableDictionary"), S("dictionary"));
    setBool(registration, "register", true);
    axui_send_dictionary(g_axui_client,
        "sendAsynchronousMessage:withIdentifier:targetAccessQueue:completion:", registration, 25);
    g_axui_register_sent = M_bool1(g_axui_client, S("respondsToSelector:"),
        (OID)S("sendAsynchronousMessage:withIdentifier:targetAccessQueue:completion:"));

    OID caption = M_id0(C("NSDictionary"), S("dictionary"));
    axui_send_dictionary(g_axui_client,
        "sendAsynchronousMessage:withIdentifier:targetAccessQueue:completion:", caption, 8);
    g_axui_caption_sent = g_axui_register_sent;

    OID curtain = M_id0(C("NSMutableDictionary"), S("dictionary"));
    setBool(curtain, "enabled", false);
    axui_send_dictionary(g_axui_client,
        "sendAsynchronousMessage:withIdentifier:targetAccessQueue:completion:", curtain, 7);
    g_axui_curtain_disabled_sent = g_axui_register_sent;
    if (g_axui_register_sent) g_axui_bootstrap_error[0] = 0;
    else snprintf(g_axui_bootstrap_error, sizeof(g_axui_bootstrap_error), "AXUIClient message API unavailable");
    ax_log("AXUIClient created=%d delegate=%d registered=%d curtainDisabledMessage=%d",
        g_axui_client_created, g_axui_delegate_set, g_axui_register_sent, g_axui_curtain_disabled_sent);
}

static void axui_wait_for_server_ready(void) {
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 50000000 };
    for (int attempt = 0; attempt < 20 && !g_axui_server_ready; attempt++) nanosleep(&pause, 0);
}

static OID axui_bootstrap_status(void) {
    OID status = M_id0(C("NSMutableDictionary"), S("dictionary"));
    setBool(status, "ok", g_axui_client_created && g_axui_register_sent);
    setBool(status, "attempted", g_axui_bootstrap_attempted != 0);
    setBool(status, "accessibilityUIFrameworkLoaded", g_axui_framework_loaded != 0);
    setBool(status, "displayManagerPrimed", g_axui_display_manager_primed != 0);
    setBool(status, "axUIClientCreated", g_axui_client_created != 0);
    setBool(status, "delegateCreated", g_axui_delegate_created != 0);
    setBool(status, "delegateSet", g_axui_delegate_set != 0);
    setBool(status, "registerMessageSent", g_axui_register_sent != 0);
    setBool(status, "captionMessageSent", g_axui_caption_sent != 0);
    setBool(status, "curtainDisabledMessageSent", g_axui_curtain_disabled_sent != 0);
    setBool(status, "uiServerReady", g_axui_server_ready != 0);
    setBool(status, "voiceOverTouchInteractionChanged", false);
    if (g_axui_application_accessibility_before >= 0)
        setBool(status, "applicationAccessibilityEnabledBefore", g_axui_application_accessibility_before != 0);
    if (g_axui_application_accessibility_after >= 0)
        setBool(status, "applicationAccessibilityEnabledAfter", g_axui_application_accessibility_after != 0);
    if (g_axui_voiceover_usage_before >= 0)
        setBool(status, "voiceOverUsageConfirmedBefore", g_axui_voiceover_usage_before != 0);
    if (g_axui_voiceover_usage_after >= 0)
        setBool(status, "voiceOverUsageConfirmedAfter", g_axui_voiceover_usage_after != 0);
    if (g_axui_voiceover_touch_before >= 0)
        setBool(status, "voiceOverTouchEnabledBefore", g_axui_voiceover_touch_before != 0);
    if (g_axui_voiceover_touch_after >= 0)
        setBool(status, "voiceOverTouchEnabledAfter", g_axui_voiceover_touch_after != 0);
    if (g_axui_bootstrap_error[0]) dset(status, "error", NS(g_axui_bootstrap_error));
    return status;
}

static int axui_bootstrap(void) {
    if (mdc_run_on_main_thread_sync(axui_bootstrap_main, 0) != 0) {
        snprintf(g_axui_bootstrap_error, sizeof(g_axui_bootstrap_error), "main-thread AX bootstrap bridge unavailable");
        return -1;
    }
    axui_wait_for_server_ready();
    return g_axui_client_created && g_axui_register_sent ? 0 : -1;
}

typedef struct { AXRef element; AXRef attribute; AXRef value; AXError error; int names; } AXQueryCtx;
static void ax_query_main(void *p) {
    AXQueryCtx *query = p;
    if (!g_ax_query_route_logged) {
        ax_log("AX attribute calls route=main setter=%d override=%d",
            F_setRequestingClient != 0,
            !F_setRequestingClient && F_overrideRequestingClientType != 0);
        g_ax_query_route_logged = 1;
    }
    if (F_setMessagingTimeout) F_setMessagingTimeout(query->element, AX_TIMEOUT_SECONDS);
    if (F_setRequestingClient) F_setRequestingClient(2);
    uint64_t prior_type = 0;
    int overridden = !F_setRequestingClient && F_overrideRequestingClientType;
    if (overridden) prior_type = F_overrideRequestingClientType(2);
    if (query->names) {
        query->error = F_copyAttributeNames
            ? F_copyAttributeNames(query->element, &query->value) : -1;
    } else {
        query->error = F_copyAttributeValue
            ? F_copyAttributeValue(query->element, query->attribute, &query->value) : -1;
    }
    if (overridden) F_overrideRequestingClientType(prior_type);
}

static AXError ax_copy_attribute_value(AXRef element, AXRef attribute, AXRef *value) {
    AXQueryCtx query = { element, attribute, 0, -1, 0 };
    if (mdc_run_on_main_thread_sync(ax_query_main, &query) != 0) return -1;
    if (value) *value = query.value;
    return query.error;
}

static AXError ax_copy_attribute_names(AXRef element, AXRef *names) {
    AXQueryCtx query = { element, 0, 0, -1, 1 };
    if (mdc_run_on_main_thread_sync(ax_query_main, &query) != 0) return -1;
    if (names) *names = query.value;
    return query.error;
}

static void dset(OID dict, const char *key, OID obj) {
    if (!obj) return;
    OID k = NS(key);
    typedef OID (*F2)(OID, OSEL, OID, OID);
    ((F2)(void *)M_id1)(dict, S("setObject:forKey:"), obj, k);
    M_void0(k, S("release"));
}

static void setInt(OID dict, const char *key, int v) {
    dset(dict, key, M_numi(C("NSNumber"), S("numberWithInt:"), v));
}

static void setBool(OID dict, const char *key, bool v) {
    dset(dict, key, M_numi(C("NSNumber"), S("numberWithBool:"), v ? 1 : 0));
}

typedef struct { int pid; int stage; int has_front; int system_root; char bundle[256]; } FrontAppInfo;

static void read_front_app_main(void *p) {
    FrontAppInfo *info = p;
    OID springboardClass = C("SpringBoard");
    OID uiApp = springboardClass ? M_id0(springboardClass, S("sharedApplication")) : 0;
    if (!uiApp) uiApp = M_id0(C("UIApplication"), S("sharedApplication"));
    if (!uiApp) info->stage = 1;
    OID frontSelector = (OID)S("_accessibilityFrontMostApplication");
    OID front = uiApp && M_bool1(uiApp, S("respondsToSelector:"), frontSelector)
        ? M_id0(uiApp, (OSEL)frontSelector) : 0;
    if (front) {
        info->has_front = 1;
        // SBApplication has no `pid` selector on all supported iOS builds.
        // Probe the runtime before invoking it; this used to abort SpringBoard
        // on iOS 17 when Notes was the foreground app.
        static const char *pidSelectors[] = { "pid", "processIdentifier", "processID" };
        for (size_t i = 0; i < sizeof(pidSelectors) / sizeof(pidSelectors[0]) && info->pid <= 0; i++) {
            OSEL selector = S(pidSelectors[i]);
            if (M_bool1(front, S("respondsToSelector:"), (OID)selector))
                info->pid = M_int(front, selector);
        }
        OSEL bundleSelector = S("bundleIdentifier");
        OID bundle = M_bool1(front, S("respondsToSelector:"), (OID)bundleSelector)
            ? M_id0(front, bundleSelector) : 0;
        const char *utf8 = bundle ? M_cstr(bundle, S("UTF8String")) : 0;
        if (utf8) snprintf(info->bundle, sizeof(info->bundle), "%s", utf8);
    }
    if (info->pid <= 0 && !info->stage) info->stage = info->has_front ? 3 : 2;
}

static void read_front_app_sbs(FrontAppInfo *info) {
    if (!F_copyFrontmostDisplayId || !F_processIdForDisplayId || !F_CFRelease) return;
    OID displayId = F_copyFrontmostDisplayId();
    if (!displayId) return;
    info->has_front = 1;
    uint32_t pid = 0;
    int resolved = F_processIdForDisplayId(displayId, &pid);
    if (resolved && pid > 0) info->pid = (int)pid;
    const char *utf8 = M_cstr(displayId, S("UTF8String"));
    if (utf8) snprintf(info->bundle, sizeof(info->bundle), "%s", utf8);
    F_CFRelease(displayId);
    if (info->pid <= 0) info->stage = 3;
}

static int front_app_info(FrontAppInfo *info) {
    memset(info, 0, sizeof(*info));
    ax_log("front lookup begin");
    if (mdc_run_on_main_thread_sync(read_front_app_main, info) != 0)
        info->stage = 4;
    ax_log("front accessor pid=%d has=%d stage=%d bundle=%s", info->pid, info->has_front, info->stage, info->bundle);
    if (info->pid <= 0) read_front_app_sbs(info);
    ax_log("SBS fallback pid=%d has=%d stage=%d bundle=%s", info->pid, info->has_front, info->stage, info->bundle);
    if (info->pid <= 0) {
        // Home and system-owned screens have no frontmost SBApplication.
        // Query SpringBoard itself so agents can still inspect app icons.
        info->pid = (int)getpid();
        info->system_root = 1;
        snprintf(info->bundle, sizeof(info->bundle), "com.apple.springboard");
    }
    return 0;
}

typedef struct { const FrontAppInfo *info; AXRef ref; } MakeFrontAXCtx;
static void make_front_ax_ref_main(void *p) {
    MakeFrontAXCtx *ctx = p;
    const FrontAppInfo *info = ctx->info;
    if (!info || info->pid <= 0) return;
    if (F_setRequestingClient) F_setRequestingClient(2);
    int own_pid = (int)getpid();
    if (F_addAssociatedPid) {
        F_addAssociatedPid(own_pid, info->pid, 0);
        F_addAssociatedPid(info->pid, own_pid, 0);
        F_addAssociatedPid(own_pid, info->pid, 1);
        F_addAssociatedPid(info->pid, own_pid, 1);
    }
    if (F_isPidAssociated) {
        ax_log("AX association local=%d remote=%d localAssociated=%d remoteAssociated=%d localDisplay1=%d remoteDisplay1=%d",
            own_pid, info->pid, F_isPidAssociated(own_pid), F_isPidAssociated(info->pid),
            F_isPidAssociatedWithDisplayType ? F_isPidAssociatedWithDisplayType(own_pid, 1) : -1,
            F_isPidAssociatedWithDisplayType ? F_isPidAssociatedWithDisplayType(info->pid, 1) : -1);
    }
    ax_log("create app AX root pid=%d", info->pid);
    AXRef ref = F_createApplication(info->pid);
    ax_log("create app AX root done present=%d", ref != 0);
    if (!ref) return;
    F_setMessagingTimeout(ref, AX_TIMEOUT_SECONDS);
    if (F_getPid) {
        int pid = 0;
        AXError error = F_getPid(ref, &pid);
        ax_log("created AX root getpid error=%d pid=%d", error, pid);
    }
    ctx->ref = ref;
}

static AXRef make_front_ax_ref(const FrontAppInfo *info) {
    MakeFrontAXCtx ctx = { info, 0 };
    if (mdc_run_on_main_thread_sync(make_front_ax_ref_main, &ctx) != 0)
        ax_log("AX main-thread bridge unavailable for app root");
    return ctx.ref;
}

typedef struct { AXRef ref; } CreateSystemWideCtx;
static void create_systemwide_main(void *p) {
    CreateSystemWideCtx *ctx = p;
    if (F_createSystemWide) ctx->ref = F_createSystemWide();
    if (ctx->ref && F_setMessagingTimeout) F_setMessagingTimeout(ctx->ref, AX_TIMEOUT_SECONDS);
}

static AXRef create_systemwide_ref(void) {
    CreateSystemWideCtx ctx = { 0 };
    if (mdc_run_on_main_thread_sync(create_systemwide_main, &ctx) != 0) return 0;
    return ctx.ref;
}

static OID ax_copy_named_attribute(AXRef element, const char *name, AXError *error) {
    if (!element || !F_copyAttributeValue || query_deadline_reached()) return 0;
    AXRef value = 0;
    static const struct { const char *name; const char *xc_symbol; const char *symbol; } constants[] = {
        {"AXLabel", "kAXXCAttributeLabel", "kAXLabelAttribute"},
        {"AXIdentifier", "kAXXCAttributeIdentifier", "kAXIdentifierAttribute"},
        {"AXRole", 0, "kAXRoleAttribute"}, {"AXTitle", 0, "kAXTitleAttribute"},
        {"AXValue", "kAXXCAttributeValue", "kAXValueAttribute"},
        {"AXTraits", "kAXXCAttributeTraits", "kAXTraitsAttribute"},
        {"AXFrame", "kAXXCAttributeFrame", "kAXFrameAttribute"},
        {"AXChildren", "kAXXCAttributeChildren", "kAXChildrenAttribute"},
        {"AXVisibleChildren", 0, "kAXVisibleChildrenAttribute"},
        {"AXWindows", 0, "kAXWindowsAttribute"}, {"AXElements", 0, "kAXElementsAttribute"},
        {"AXFocusedWindow", 0, "kAXFocusedWindowAttribute"},
        {"AXMainWindow", 0, "kAXMainWindowAttribute"}
    };
    OID key = 0;
    const char *selected_symbol = "literal-string";
    for (size_t i = 0; i < sizeof(constants) / sizeof(constants[0]); i++) {
        if (strcmp(name, constants[i].name) != 0) continue;
        const char *symbols[] = {constants[i].xc_symbol, constants[i].symbol};
        for (size_t j = 0; j < sizeof(symbols) / sizeof(symbols[0]) && !key; j++) {
            if (!symbols[j]) continue;
            void *symbol = ax_symbol(symbols[j]);
            if (symbol) { key = *(OID *)symbol; selected_symbol = symbols[j]; }
        }
        break;
    }
    // iOS-MCP prefers the exported XCTest AX attribute objects and falls back
    // to standard AX attribute objects. Keep literal strings for runtimes that
    // export neither symbol.
    if (!key) key = NS(name);
    if (g_ax_attribute_log_count < 12) {
        g_ax_attribute_log_count++;
        ax_log("AX attribute ref name=%s source=%s", name, selected_symbol);
    }
    *error = ax_copy_attribute_value(element, (AXRef)key, &value);
    return (OID)value;
}

static OID dump_raw_ax_element(AXRef element, int depth) {
    if (!element || query_deadline_reached() || depth > g_max_depth || g_nodes >= MAXNODES) return 0;
    g_nodes++;
    OID node = M_id0(C("NSMutableDictionary"), S("dictionary"));
    static const struct { const char *ax_name; const char *json_name; } attrs[] = {
        {"AXLabel", "label"}, {"AXIdentifier", "id"}, {"AXRole", "role"}, {"AXTitle", "title"}
    };
    for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]) && !query_deadline_reached(); i++) {
        AXError error = 0;
        OID value = ax_copy_named_attribute(element, attrs[i].ax_name, &error);
        if (depth == 0) ax_log("raw AX attribute %s error=%d present=%d", attrs[i].ax_name, error, value != 0);
        if (!error && value && M_bool1(value, S("isKindOfClass:"), C("NSString")))
            dset(node, attrs[i].json_name, value);
    }
    if (depth > 0 && !query_deadline_reached()) {
        AXError error = 0;
        OID value = ax_copy_named_attribute(element, "AXValue", &error);
        if (!error && value && (M_bool1(value, S("isKindOfClass:"), C("NSString")) ||
                               M_bool1(value, S("isKindOfClass:"), C("NSNumber"))))
            dset(node, "value", value);
    }
    if (!query_deadline_reached()) {
        AXError error = 0;
        OID value = ax_copy_named_attribute(element, "AXTraits", &error);
        if (!error && value && M_bool1(value, S("isKindOfClass:"), C("NSNumber")))
            dset(node, "traits", value);
    }
    if (!query_deadline_reached()) {
        AXError error = 0;
        OID value = ax_copy_named_attribute(element, "AXFrame", &error);
        if (!error && value && M_bool1(value, S("isKindOfClass:"), C("NSValue"))) {
            R64 frame = M_rect(value, S("CGRectValue"));
            if (frame.w > 0 && frame.h > 0) {
                OID coords = M_id0(C("NSMutableArray"), S("array"));
                typedef void (*F1)(OID, OSEL, OID);
                F1 add = (F1)(void *)M_id1;
                add(coords, S("addObject:"), M_numi(C("NSNumber"), S("numberWithInt:"), (int)frame.x));
                add(coords, S("addObject:"), M_numi(C("NSNumber"), S("numberWithInt:"), (int)frame.y));
                add(coords, S("addObject:"), M_numi(C("NSNumber"), S("numberWithInt:"), (int)frame.w));
                add(coords, S("addObject:"), M_numi(C("NSNumber"), S("numberWithInt:"), (int)frame.h));
                dset(node, "frame", coords);
            }
        }
    }
    if (query_deadline_reached()) return node;
    AXError child_error = 0;
    OID children = ax_copy_named_attribute(element, "AXChildren", &child_error);
    static const char *child_attributes[] = {
        "AXVisibleChildren", "AXWindows", "AXElements", "AXFocusedWindow", "AXMainWindow"
    };
    for (size_t i = 0; !children && !query_deadline_reached() &&
         i < sizeof(child_attributes) / sizeof(child_attributes[0]); i++) {
        children = ax_copy_named_attribute(element, child_attributes[i], &child_error);
    }
    if (children && M_bool1(children, S("isKindOfClass:"), C("NSSet")))
        children = M_id0(children, S("allObjects"));
    if (children && !M_bool1(children, S("isKindOfClass:"), C("NSArray"))) {
        if (M_bool1(children, S("respondsToSelector:"), (OID)S("allObjects")))
            children = M_id0(children, S("allObjects"));
        else {
            OID one_child = children;
            children = M_id0(C("NSMutableArray"), S("array"));
            typedef void (*F1)(OID, OSEL, OID);
            ((F1)(void *)M_id1)(children, S("addObject:"), one_child);
        }
    }
    if (depth == 0) ax_log("raw AX children error=%d present=%d", child_error, children != 0);
    if (children && M_bool1(children, S("isKindOfClass:"), C("NSArray"))) {
        unsigned long count = M_count(children, S("count"));
        if (depth == 0) ax_log("raw AX child count=%lu", count);
        if (count > MAXNODES) count = MAXNODES;
        if (count > 0) {
            OID array = M_id0(C("NSMutableArray"), S("array"));
            typedef void (*F1)(OID, OSEL, OID);
            F1 add = (F1)(void *)M_id1;
            for (unsigned long i = 0; i < count && g_nodes < MAXNODES && !query_deadline_reached(); i++) {
                AXRef child = (AXRef)M_at(children, S("objectAtIndex:"), i);
                OID child_node = dump_raw_ax_element(child, depth + 1);
                if (child_node) add(array, S("addObject:"), child_node);
            }
            if (M_count(array, S("count")) > 0) dset(node, "children", array);
        }
    }
    return node;
}

typedef struct { uint32_t id; const char *name; } NumericCandidateAttribute;
static const NumericCandidateAttribute kCandidateAttributes[] = {
    {3015, "visibleElements"},
    {3025, "elementsWithSemanticContext"},
    {3022, "explorerElements"},
    {3029, "nativeFocusableElements"},
    {3031, "siriContentNativeFocusableElements"},
    {3032, "siriContentElementsWithSemanticContext"},
    {5001, "privateChildren"}
};

// iOS AXRuntime's numeric attributes are passed as small pointer-valued
// attribute references. Keep direct AXRuntime access on SpringBoard's main
// thread: iOS 17 aborts SpringBoard if its AX callback is entered from a
// background thread.
static OID dump_raw_numeric_candidates(AXRef source, OID diagnostics, const char *diagnostic_key) {
    if (!source || !F_copyAttributeValue || query_deadline_reached()) return 0;
    OID source_counts = M_id0(C("NSMutableDictionary"), S("dictionary"));
    dset(diagnostics, diagnostic_key, source_counts);
    for (size_t a = 0; a < sizeof(kCandidateAttributes) / sizeof(kCandidateAttributes[0]); a++) {
        if (query_deadline_reached()) break;
        AXRef raw = 0;
        ax_log("numeric AX query source=%s attr=%s id=%u begin", diagnostic_key, kCandidateAttributes[a].name, kCandidateAttributes[a].id);
        AXError error = ax_copy_attribute_value(source, (AXRef)(uintptr_t)kCandidateAttributes[a].id, &raw);
        ax_log("numeric AX query source=%s attr=%s id=%u done error=%d present=%d", diagnostic_key, kCandidateAttributes[a].name, kCandidateAttributes[a].id, error, raw != 0);
        OID summary = M_id0(C("NSMutableDictionary"), S("dictionary"));
        setInt(summary, "error", error);
        if (!raw || error != 0) {
            setInt(summary, "count", 0);
            dset(source_counts, kCandidateAttributes[a].name, summary);
            if (raw) F_CFRelease(raw);
            continue;
        }

        OID values = 0;
        if (M_bool1((OID)raw, S("isKindOfClass:"), C("NSArray"))) values = (OID)raw;
        else if (M_bool1((OID)raw, S("isKindOfClass:"), C("NSSet"))) values = M_id0((OID)raw, S("allObjects"));
        unsigned long count = values ? M_count(values, S("count")) : 0;
        ax_log("numeric AX query source=%s attr=%s count=%lu", diagnostic_key, kCandidateAttributes[a].name, count);
        setInt(summary, "count", (int)(count > 2147483647ul ? 2147483647 : count));
        dset(source_counts, kCandidateAttributes[a].name, summary);

        if (count > 0) {
            OID tree = M_id0(C("NSMutableDictionary"), S("dictionary"));
            OID array = M_id0(C("NSMutableArray"), S("array"));
            typedef void (*F1)(OID, OSEL, OID);
            F1 add = (F1)(void *)M_id1;
            unsigned long limit = count > MAXNODES ? MAXNODES : count;
            for (unsigned long i = 0; i < limit && g_nodes < MAXNODES && !query_deadline_reached(); i++) {
                AXRef raw_element = (AXRef)M_at(values, S("objectAtIndex:"), i);
                if (!raw_element) continue;
                OID child = dump_raw_ax_element(raw_element, 0);
                if (child) add(array, S("addObject:"), child);
            }
            if (M_count(array, S("count")) > 0) {
                dset(tree, "children", array);
                if (raw) F_CFRelease(raw);
                return tree;
            }
        }
        if (raw) F_CFRelease(raw);
    }
    return 0;
}

static int serialize_json(OID root, char **out, size_t *outlen) {
    void *err = 0;
    OID data = M_json(C("NSJSONSerialization"), S("dataWithJSONObject:options:error:"), root, 0, &err);
    if (!data) return -1;
    size_t len = M_count(data, S("length"));
    const void *bytes = M_bytes(data, S("bytes"));
    if (!len || !bytes) return -1;
    char *buf = malloc(len);
    if (!buf) return -1;
    memcpy(buf, bytes, len);
    *out = buf;
    *outlen = len;
    return 0;
}

static int ax_dump_impl(char **out, size_t *outlen, int depth) {
    *out = 0; *outlen = 0;
    if (depth < 1 || depth > MAXDEPTH || ax_init() != 0) return -1;
    ax_log_reset();
    ax_log("AX dump begin depth=%d", depth);
    ax_log("AX runtime caps image=%d app=%d system=%d copy=%d timeout=%d requestor=%d requestorOverride=%d associate=%d associationCheck=%d getpid=%d",
        G_AX_RUNTIME_INDEX,
        F_createApplication != 0, F_createSystemWide != 0, F_copyAttributeValue != 0,
        F_setMessagingTimeout != 0, F_setRequestingClient != 0, F_overrideRequestingClientType != 0,
        F_addAssociatedPid != 0, F_isPidAssociated != 0, F_getPid != 0);
    OID pool = begin_pool();
    int bootstrap_rc = axui_bootstrap();
    ax_log("AXUIClient bootstrap rc=%d client=%d ready=%d appAX=%d->%d usage=%d->%d",
        bootstrap_rc, g_axui_client_created, g_axui_server_ready,
        g_axui_application_accessibility_before, g_axui_application_accessibility_after,
        g_axui_voiceover_usage_before, g_axui_voiceover_usage_after);
    FrontAppInfo front;
    if (front_app_info(&front) != 0) {
        OID root = M_id0(C("NSMutableDictionary"), S("dictionary"));
        dset(root, "error", NS("ax-no-front-app"));
        setInt(root, "stage", front.stage);
        setInt(root, "frontmostAvailable", front.has_front);
        setInt(root, "pid", front.pid);
        dset(root, "axBootstrap", axui_bootstrap_status());
        int rc = serialize_json(root, out, outlen);
        if (pool) M_void0(pool, S("drain"));
        return rc;
    }
    ax_log("AX front selected pid=%d bundle=%s system=%d", front.pid, front.bundle, front.system_root);
    g_max_depth = depth;
    g_deadline_ns = monotonic_ns() + QUERY_BUDGET_NS;
    g_nodes = 0;
    AXRef app_ref = make_front_ax_ref(&front);
    if (!app_ref) {
        OID root = M_id0(C("NSMutableDictionary"), S("dictionary"));
        dset(root, "error", NS("ax-app-element-unavailable"));
        setInt(root, "pid", front.pid);
        if (front.bundle[0]) dset(root, "app", NS(front.bundle));
        dset(root, "axBootstrap", axui_bootstrap_status());
        if (app_ref) F_CFRelease(app_ref);
        int rc = serialize_json(root, out, outlen);
        if (pool) M_void0(pool, S("drain"));
        return rc;
    }
    OID root = M_id0(C("NSMutableDictionary"), S("dictionary"));
    if (front.bundle[0]) dset(root, "app", NS(front.bundle));
    setInt(root, "pid", front.pid);
    setInt(root, "frontmostAvailable", front.has_front);
    setInt(root, "systemRoot", front.system_root);
    dset(root, "axBootstrap", axui_bootstrap_status());
    OID candidate_diagnostics = M_id0(C("NSMutableDictionary"), S("dictionary"));
    OID tree = 0;
    if (front.system_root) {
        AXRef system_ref = create_systemwide_ref();
        if (system_ref) {
            tree = dump_raw_numeric_candidates(system_ref, candidate_diagnostics, "systemWideNumericCandidateCounts");
            F_CFRelease(system_ref);
        }
    }
    if (!tree && app_ref && !query_deadline_reached())
        tree = dump_raw_numeric_candidates(app_ref, candidate_diagnostics, "appNumericCandidateCounts");
    if (!tree && app_ref && !query_deadline_reached()) {
        AXRef names = 0;
        AXError names_error = ax_copy_attribute_names(app_ref, &names);
        unsigned long name_count = names && M_bool1((OID)names, S("isKindOfClass:"), C("NSArray"))
            ? M_count((OID)names, S("count")) : 0;
        ax_log("raw AX attribute names error=%d count=%lu", names_error, name_count);
        OID summary = M_id0(C("NSMutableDictionary"), S("dictionary"));
        setInt(summary, "error", names_error);
        setInt(summary, "count", (int)name_count);
        dset(root, "rawAXAttributeNames", summary);
        tree = dump_raw_ax_element(app_ref, 0);
    }
    if (!tree && app_ref && !query_deadline_reached()) tree = dump_raw_ax_element(app_ref, 0);
    dset(root, "axCandidateDiagnostics", candidate_diagnostics);
    setInt(root, "requestingClientOverride", !F_setRequestingClient && F_overrideRequestingClientType);
    if (tree) dset(root, "root", tree);
    setInt(root, "nodes", g_nodes);
    setInt(root, "timedOut", query_deadline_reached() ? 1 : 0);
    if (!tree) dset(root, "error", NS(query_deadline_reached() ? "ax-timeout" : "ax-tree-empty"));
    int rc = serialize_json(root, out, outlen);
    ax_log("AX dump end nodes=%d timedOut=%d tree=%d", g_nodes, query_deadline_reached() ? 1 : 0, tree != 0);
    if (app_ref) F_CFRelease(app_ref);
    if (pool) M_void0(pool, S("drain"));
    return rc;
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    int waiter_active;
    int done;
    int depth;
    int rc;
    char *out;
    size_t outlen;
} AxDumpJob;

static void free_ax_job(AxDumpJob *job) {
    if (!job) return;
    free(job->out);
    pthread_cond_destroy(&job->ready);
    pthread_mutex_destroy(&job->lock);
    free(job);
}

static void *ax_dump_worker(void *arg) {
    AxDumpJob *job = arg;
    int rc = ax_dump_impl(&job->out, &job->outlen, job->depth);
    pthread_mutex_lock(&g_ax_busy_lock);
    g_ax_busy = 0;
    pthread_mutex_unlock(&g_ax_busy_lock);
    pthread_mutex_lock(&job->lock);
    job->rc = rc;
    job->done = 1;
    int waiter_active = job->waiter_active;
    if (waiter_active) pthread_cond_signal(&job->ready);
    pthread_mutex_unlock(&job->lock);
    if (!waiter_active) free_ax_job(job);
    return 0;
}

// A hung private AX call may outlive its per-element messaging timeout on some
// iOS builds. Keep it off SpringBoard and let the command server return in 6s.
int ax_dump(char **out, size_t *outlen, int depth) {
    *out = 0;
    *outlen = 0;
    pthread_mutex_lock(&g_ax_busy_lock);
    if (g_ax_busy) {
        pthread_mutex_unlock(&g_ax_busy_lock);
        return -3;
    }
    g_ax_busy = 1;
    pthread_mutex_unlock(&g_ax_busy_lock);

    AxDumpJob *job = calloc(1, sizeof(*job));
    if (!job) {
        pthread_mutex_lock(&g_ax_busy_lock);
        g_ax_busy = 0;
        pthread_mutex_unlock(&g_ax_busy_lock);
        return -1;
    }
    pthread_mutex_init(&job->lock, 0);
    pthread_cond_init(&job->ready, 0);
    job->depth = depth;
    job->waiter_active = 1;
    pthread_t worker;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int create_rc = pthread_create(&worker, &attr, ax_dump_worker, job);
    pthread_attr_destroy(&attr);
    if (create_rc != 0) {
        pthread_mutex_lock(&g_ax_busy_lock);
        g_ax_busy = 0;
        pthread_mutex_unlock(&g_ax_busy_lock);
        job->waiter_active = 0;
        free_ax_job(job);
        return -1;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 6;
    pthread_mutex_lock(&job->lock);
    int wait_rc = 0;
    while (!job->done && wait_rc == 0)
        wait_rc = pthread_cond_timedwait(&job->ready, &job->lock, &deadline);
    if (job->done) {
        int rc = job->rc;
        *out = job->out;
        *outlen = job->outlen;
        job->out = 0;
        job->waiter_active = 0;
        pthread_mutex_unlock(&job->lock);
        free_ax_job(job);
        return rc;
    }
    job->waiter_active = 0;
    pthread_mutex_unlock(&job->lock);
    return -4;
}

int ax_insert_text(const char *text) {
    (void)text;
    // Text entry through the old AXElement wrapper was not main-thread safe.
    // Keep the protocol fail-closed until a raw, main-thread insertion path is
    // implemented and can be verified on-device.
    return -1;
}
