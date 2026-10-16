/* Brief, non-interactive tap markers drawn above the SpringBoard display. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdbool.h>
#include "mdc_mainthread.h"

typedef void *Obj;
typedef void *Sel;
typedef struct { double x, y; } MdcPoint;
typedef struct { double width, height; } MdcSize;
typedef struct { MdcPoint origin; MdcSize size; } MdcRect;

static Obj (*get_class)(const char *);
static Sel (*register_sel)(const char *);
static Obj (*send_obj0)(Obj, Sel);
static Obj (*send_obj1)(Obj, Sel, Obj);
static Obj (*send_obj_index)(Obj, Sel, unsigned long);
static Obj (*send_obj_rect)(Obj, Sel, MdcRect);
static Obj (*send_obj_double4)(Obj, Sel, double, double, double, double);
static void (*send_void_obj)(Obj, Sel, Obj);
static void (*send_void_bool)(Obj, Sel, bool);
static void (*send_void_double)(Obj, Sel, double);
static void (*send_void_rect)(Obj, Sel, MdcRect);
static void (*send_void0)(Obj, Sel);
static MdcRect (*send_rect0)(Obj, Sel);
static int (*send_int0)(Obj, Sel);
static unsigned long (*send_unsigned0)(Obj, Sel);
static bool (*send_bool_obj)(Obj, Sel, Obj);
static int initialized;
static Obj overlay_window;
static Obj overlay_root;
static Obj markers[12];
static size_t marker_count;

static Sel S(const char *name) { return register_sel(name); }

static int init_runtime(void) {
    if (initialized) return 0;
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY);
    if (!objc) return -1;
    get_class = dlsym(objc, "objc_getClass");
    register_sel = dlsym(objc, "sel_registerName");
    void *msg = dlsym(objc, "objc_msgSend");
    if (!get_class || !register_sel || !msg) return -1;
    send_obj0 = msg;
    send_obj1 = msg;
    send_obj_index = msg;
    send_obj_rect = msg;
    send_obj_double4 = msg;
    send_void_obj = msg;
    send_void_bool = msg;
    send_void_double = msg;
    send_void_rect = msg;
    send_void0 = msg;
    send_rect0 = msg;
    send_int0 = msg;
    send_unsigned0 = msg;
    send_bool_obj = msg;
    initialized = 1;
    return 0;
}

typedef struct { double x, y, width, height; int shown; } TapCtx;

static void show_on_main(void *p) {
    TapCtx *tap = (TapCtx *)p;
    Obj screen_class = get_class("UIScreen");
    Obj window_class = get_class("UIWindow");
    Obj controller_class = get_class("UIViewController");
    Obj view_class = get_class("UIView");
    Obj color_class = get_class("UIColor");
    if (!screen_class || !window_class || !controller_class || !view_class || !color_class) return;

    Obj screen = send_obj0(screen_class, S("mainScreen"));
    if (!screen) return;
    MdcRect bounds = send_rect0(screen, S("bounds"));
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return;

    if (!overlay_window) {
        overlay_window = send_obj0(window_class, S("alloc"));
        // SpringBoard on iOS 13+ owns its windows through UIWindowScenes.
        // Attaching to the active scene makes this overlay visible on the
        // current display without making it key or interactive.
        Obj application = send_obj0(get_class("UIApplication"), S("sharedApplication"));
        Obj scenes = application ? send_obj0(application, S("connectedScenes")) : 0;
        Obj scene_array = scenes ? send_obj0(scenes, S("allObjects")) : 0;
        Obj scene = 0;
        unsigned long count = scene_array ? send_unsigned0(scene_array, S("count")) : 0;
        Obj scene_class = get_class("UIWindowScene");
        for (unsigned long i = 0; i < count && !scene; i++) {
            Obj candidate = send_obj_index(scene_array, S("objectAtIndex:"), i);
            if (!candidate || (scene_class && !send_bool_obj(candidate, S("isKindOfClass:"), scene_class))) continue;
            int activation = send_int0(candidate, S("activationState"));
            Obj candidate_screen = send_obj0(candidate, S("screen"));
            bool same_screen = candidate_screen == screen || send_bool_obj(candidate_screen, S("isEqual:"), screen);
            if (activation == 0 && same_screen) scene = candidate; // foreground active
        }
        if (scene) {
            overlay_window = send_obj1(overlay_window, S("initWithWindowScene:"), scene);
            if (overlay_window) send_void_rect(overlay_window, S("setFrame:"), bounds);
        } else {
            overlay_window = send_obj_rect(overlay_window, S("initWithFrame:"), bounds);
        }
        if (!overlay_window) return;
        Obj clear = send_obj0(color_class, S("clearColor"));
        Obj controller = send_obj0(controller_class, S("alloc"));
        controller = send_obj0(controller, S("init"));
        if (!controller) return;
        send_void_obj(overlay_window, S("setRootViewController:"), controller);
        overlay_root = send_obj0(controller, S("view"));
        send_void0(controller, S("release"));
        if (!overlay_root) return;
        send_void_obj(overlay_window, S("setBackgroundColor:"), clear);
        send_void_bool(overlay_window, S("setOpaque:"), false);
        send_void_bool(overlay_window, S("setUserInteractionEnabled:"), false);
        send_void_double(overlay_window, S("setWindowLevel:"), 10000.0);
        send_void_obj(overlay_root, S("setBackgroundColor:"), clear);
        send_void_bool(overlay_root, S("setUserInteractionEnabled:"), false);
        send_void_bool(overlay_root, S("setAccessibilityElementsHidden:"), true);
        send_void_bool(overlay_window, S("setHidden:"), false);
    }

    double x = tap->x * bounds.size.width / tap->width;
    double y = tap->y * bounds.size.height / tap->height;
    const double diameter = 64.0;
    MdcRect frame = {{x - diameter / 2, y - diameter / 2}, {diameter, diameter}};
    Obj marker = send_obj0(view_class, S("alloc"));
    marker = send_obj_rect(marker, S("initWithFrame:"), frame);
    if (!marker) return;
    send_void_bool(marker, S("setUserInteractionEnabled:"), false);
    send_void_bool(marker, S("setAccessibilityElementsHidden:"), true);

    Obj ring_color = send_obj_double4(color_class, S("colorWithRed:green:blue:alpha:"), 0.05, 0.82, 1.0, 0.96);
    Obj fill_color = send_obj_double4(color_class, S("colorWithRed:green:blue:alpha:"), 0.03, 0.48, 0.92, 0.28);
    Obj layer = send_obj0(marker, S("layer"));
    send_void_double(layer, S("setCornerRadius:"), diameter / 2);
    send_void_double(layer, S("setBorderWidth:"), 3.5);
    send_void_obj(layer, S("setBorderColor:"), send_obj0(ring_color, S("CGColor")));
    send_void_obj(marker, S("setBackgroundColor:"), fill_color);

    Obj inner_ring = send_obj0(view_class, S("alloc"));
    const double inner_diameter = 34.0;
    MdcRect inner_frame = {{(diameter - inner_diameter) / 2, (diameter - inner_diameter) / 2},
                           {inner_diameter, inner_diameter}};
    inner_ring = send_obj_rect(inner_ring, S("initWithFrame:"), inner_frame);
    if (inner_ring) {
        send_void_bool(inner_ring, S("setUserInteractionEnabled:"), false);
        send_void_bool(inner_ring, S("setAccessibilityElementsHidden:"), true);
        Obj inner_layer = send_obj0(inner_ring, S("layer"));
        send_void_double(inner_layer, S("setCornerRadius:"), inner_diameter / 2);
        send_void_double(inner_layer, S("setBorderWidth:"), 2.0);
        send_void_obj(inner_layer, S("setBorderColor:"), send_obj0(ring_color, S("CGColor")));
        send_void_obj(inner_ring, S("setBackgroundColor:"), send_obj0(color_class, S("clearColor")));
        send_void_obj(marker, S("addSubview:"), inner_ring);
        send_void0(inner_ring, S("release"));
    }

    Obj dot = send_obj0(view_class, S("alloc"));
    const double dot_diameter = 10.0;
    MdcRect dot_frame = {{(diameter - dot_diameter) / 2, (diameter - dot_diameter) / 2},
                         {dot_diameter, dot_diameter}};
    dot = send_obj_rect(dot, S("initWithFrame:"), dot_frame);
    if (dot) {
        send_void_bool(dot, S("setUserInteractionEnabled:"), false);
        Obj dot_layer = send_obj0(dot, S("layer"));
        send_void_double(dot_layer, S("setCornerRadius:"), dot_diameter / 2);
        send_void_obj(dot, S("setBackgroundColor:"), ring_color);
        send_void_obj(marker, S("addSubview:"), dot);
        send_void0(dot, S("release"));
    }

    if (marker_count == sizeof(markers) / sizeof(markers[0])) {
        Obj oldest = markers[0];
        send_void0(oldest, S("removeFromSuperview"));
        send_void0(oldest, S("release"));
        for (size_t i = 1; i < marker_count; i++) markers[i - 1] = markers[i];
        marker_count--;
    }
    send_void_obj(overlay_root, S("addSubview:"), marker);
    markers[marker_count++] = marker;
    tap->shown = 1;
}

int mdc_show_tap(double x, double y, double width, double height) {
    if (init_runtime() != 0 || width <= 0 || height <= 0) return 0;
    TapCtx tap = { x, y, width, height, 0 };
    if (mdc_run_on_main_thread_sync(show_on_main, &tap) != 0) return 0;
    return tap.shown;
}
