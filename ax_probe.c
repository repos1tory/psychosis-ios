/* Bounded, non-mutating probe for the device's private AXRuntime C bridge. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef void *Ref;
typedef int AXError;
typedef Ref (*CreateSystemWideFn)(void);
typedef Ref (*CreatePidFn)(int);
typedef AXError (*SetTimeoutFn)(Ref, float);
typedef AXError (*CopyAttributeFn)(Ref, Ref, Ref *);
typedef Ref (*StringCreateFn)(Ref, const char *, unsigned);
typedef unsigned long (*TypeIdFn)(Ref);
typedef unsigned long (*ArrayCountFn)(Ref);
typedef void (*ReleaseFn)(Ref);
typedef unsigned long (*ArrayTypeIdFn)(void);
typedef AXError (*AtPositionFn)(Ref, Ref *, float, float);
typedef AXError (*ElementAtPositionFn)(Ref, float, float, Ref *);
typedef AXError (*GetPidFn)(Ref, int *);
typedef void (*AssociatePidFn)(int, int, int);
typedef Ref (*CopyFrontmostIdFn)(void);
typedef unsigned int (*PidForDisplayIdFn)(Ref);
typedef int (*StringGetCStringFn)(Ref, char *, long, unsigned);

static Ref make_string(StringCreateFn create, const char *text) {
    return create ? create(0, text, 0x08000100u) : 0;
}

static void describe_attribute(CopyAttributeFn copy, StringCreateFn create,
                              TypeIdFn get_type, ArrayTypeIdFn array_type,
                              ArrayCountFn count_fn, ReleaseFn release,
                              Ref element, const char *name) {
    Ref key = make_string(create, name), value = 0;
    AXError error = (copy && element && key) ? copy(element, key, &value) : -1;
    unsigned long type = (value && get_type) ? get_type(value) : 0;
    unsigned long arr_type = array_type ? array_type() : 0;
    long count = (value && type == arr_type && count_fn) ? (long)count_fn(value) : -1;
    printf("attr_%s_error=%d type=%lu array_count=%ld present=%d\n",
           name, error, type, count, value != 0);
    if (value && release) release(value);
    if (key && release) release(key);
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    int notes_pid = argc > 1 ? atoi(argv[1]) : 0;
    int springboard_pid = argc > 2 ? atoi(argv[2]) : 0;
    const char *path = "/System/Library/PrivateFrameworks/AXRuntime.framework/AXRuntime";
    void *ax = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY | RTLD_GLOBAL);
    if (!ax || !cf) { puts("runtime_unavailable=1"); return 2; }
    CreateSystemWideFn create_system = dlsym(ax, "AXUIElementCreateSystemWide");
    CreatePidFn create_pid = dlsym(ax, "_AXUIElementCreateAppElementWithPid");
    SetTimeoutFn set_timeout = dlsym(ax, "AXUIElementSetMessagingTimeout");
    CopyAttributeFn copy = dlsym(ax, "AXUIElementCopyAttributeValue");
    StringCreateFn string_create = dlsym(cf, "CFStringCreateWithCString");
    TypeIdFn get_type = dlsym(cf, "CFGetTypeID");
    ArrayTypeIdFn array_type = dlsym(cf, "CFArrayGetTypeID");
    ArrayCountFn array_count = dlsym(cf, "CFArrayGetCount");
    ReleaseFn release = dlsym(cf, "CFRelease");
    AtPositionFn app_at = dlsym(ax, "AXUIElementCopyApplicationAtPosition");
    ElementAtPositionFn element_at = dlsym(ax, "AXUIElementCopyElementAtPosition");
    GetPidFn get_pid = dlsym(ax, "AXUIElementGetPid");
    AssociatePidFn associate = dlsym(ax, "_AXAddAssociatedPid");
    printf("probe_pid=%d notes_pid=%d springboard_pid=%d\n", getpid(), notes_pid, springboard_pid);
    void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_LAZY | RTLD_GLOBAL);
    CopyFrontmostIdFn frontmost_id = sbs ? dlsym(sbs, "SBSCopyFrontmostApplicationDisplayIdentifier") : 0;
    PidForDisplayIdFn pid_for_id = sbs ? dlsym(sbs, "SBSProcessIDForDisplayIdentifier") : 0;
    StringGetCStringFn string_get = cf ? dlsym(cf, "CFStringGetCString") : 0;
    Ref display_id = frontmost_id ? frontmost_id() : 0;
    char display_name[256] = {0};
    if (display_id && string_get) string_get(display_id, display_name, sizeof(display_name), 0x08000100u);
    printf("sbs_frontmost_available=%d frontmost=%s frontmost_pid=%u\n",
           frontmost_id != 0, display_name, display_id && pid_for_id ? pid_for_id(display_id) : 0);
    if (display_id && release) release(display_id);
    if (notes_pid > 0 && associate) {
        associate((int)getpid(), notes_pid, 0);
        associate(notes_pid, (int)getpid(), 0);
    }
    if (springboard_pid > 0 && associate) {
        associate((int)getpid(), springboard_pid, 0);
        associate(springboard_pid, (int)getpid(), 0);
    }

    Ref system = create_system ? create_system() : 0;
    AXError timeout_error = system && set_timeout ? set_timeout(system, 1.0f) : -1;
    printf("systemwide=%d timeout_error=%d\n", system != 0, timeout_error);
    describe_attribute(copy, string_create, get_type, array_type, array_count, release, system, "AXFocusedApplication");
    describe_attribute(copy, string_create, get_type, array_type, array_count, release, system, "focusedApplication");

    Ref app = 0;
    AXError error = app_at && system ? app_at(system, &app, 384.0f, 512.0f) : -1;
    int app_pid = 0;
    AXError pid_error = app && get_pid ? get_pid(app, &app_pid) : -1;
    if (app && set_timeout) set_timeout(app, 1.0f);
    printf("system_app_at_error=%d app=%d pid_error=%d pid=%d\n", error, app != 0, pid_error, app_pid);
    describe_attribute(copy, string_create, get_type, array_type, array_count, release, app, "AXRole");
    describe_attribute(copy, string_create, get_type, array_type, array_count, release, app, "AXChildren");

    Ref element = 0;
    error = element_at && system ? element_at(system, 384.0f, 512.0f, &element) : -1;
    int element_pid = 0;
    pid_error = element && get_pid ? get_pid(element, &element_pid) : -1;
    if (element && set_timeout) set_timeout(element, 1.0f);
    printf("system_element_at_error=%d element=%d pid_error=%d pid=%d\n", error, element != 0, pid_error, element_pid);

    if (notes_pid > 0 && create_pid) {
        Ref notes = create_pid(notes_pid);
        if (notes && set_timeout) set_timeout(notes, 1.0f);
        int direct_pid = 0;
        pid_error = notes && get_pid ? get_pid(notes, &direct_pid) : -1;
        printf("notes_app=%d pid_error=%d pid=%d\n", notes != 0, pid_error, direct_pid);
        describe_attribute(copy, string_create, get_type, array_type, array_count, release, notes, "AXRole");
        describe_attribute(copy, string_create, get_type, array_type, array_count, release, notes, "AXChildren");
    }

    if (springboard_pid > 0 && create_pid) {
        Ref springboard = create_pid(springboard_pid);
        if (springboard && set_timeout) set_timeout(springboard, 1.0f);
        int direct_pid = 0;
        pid_error = springboard && get_pid ? get_pid(springboard, &direct_pid) : -1;
        printf("springboard_app=%d pid_error=%d pid=%d\n", springboard != 0, pid_error, direct_pid);
        describe_attribute(copy, string_create, get_type, array_type, array_count, release, springboard, "AXRole");
        Ref sb_element = 0;
        error = element_at && springboard ? element_at(springboard, 384.0f, 512.0f, &sb_element) : -1;
        printf("springboard_element_at_error=%d element=%d\n", error, sb_element != 0);
        if (sb_element && release) release(sb_element);
        if (springboard && release) release(springboard);
    }

    if (element && release) release(element);
    if (app && release) release(app);
    if (system && release) release(system);
    (void)array_count;
    return 0;
}
