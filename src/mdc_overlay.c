/* Brief, non-interactive touch markers drawn above the SpringBoard display. */
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "mdc_mainthread.h"
#include "mdc_overlay.h"
#include "mdc_protocol.h"

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
static Obj (*send_obj_double)(Obj, Sel, double);
static Obj (*send_obj_rect)(Obj, Sel, MdcRect);
static Obj (*send_obj_double4)(Obj, Sel, double, double, double, double);
static MdcPoint (*send_point_to_space)(Obj, Sel, MdcPoint, Obj);
static void (*send_void_obj)(Obj, Sel, Obj);
static void (*send_void_obj2)(Obj, Sel, Obj, Obj);
static void (*send_void_sel_obj_double)(Obj, Sel, Sel, Obj, double);
static void (*send_void_bool)(Obj, Sel, bool);
static void (*send_void_double)(Obj, Sel, double);
static void (*send_void_rect)(Obj, Sel, MdcRect);
static void (*send_void_long)(Obj, Sel, long);
static void (*send_void0)(Obj, Sel);
static MdcRect (*send_rect0)(Obj, Sel);
static int (*send_int0)(Obj, Sel);
static unsigned long (*send_unsigned0)(Obj, Sel);
static bool (*send_bool_obj)(Obj, Sel, Obj);
typedef void (*MdcMarkerCleanupIMP)(Obj, Sel);
static bool (*class_add_method)(Obj, Sel, MdcMarkerCleanupIMP, const char *);
static int initialized;
static int cleanup_method_installed;
static Obj overlay_window;
static Obj overlay_root;
static Obj status_view;
static int status_method_installed;
static Obj markers[12];
static size_t marker_count;

static Sel S(const char *name) { return register_sel(name); }

static void marker_cleanup(Obj marker, Sel selector) {
    (void)selector;
    for (size_t i = 0; i < marker_count; i++) {
        if (markers[i] != marker) continue;
        for (size_t j = i + 1; j < marker_count; j++) markers[j - 1] = markers[j];
        markers[--marker_count] = 0;
        break;
    }
    send_void0(marker, S("removeFromSuperview"));
    send_void0(marker, S("release"));
}

static void status_hide_if(Obj view, Sel selector) {
    (void)selector;
    if (status_view != view) return;
    send_void0(view, S("removeFromSuperview"));
    send_void0(view, S("release"));
    status_view = 0;
}

static Obj string_from_utf8(const char *value) {
    Obj string_class = get_class("NSString");
    return string_class ? send_obj1(string_class, S("stringWithUTF8String:"), (Obj)value) : 0;
}

static void fade_and_schedule_removal(Obj marker) {
    Obj animation_class = get_class("CABasicAnimation");
    Obj number_class = get_class("NSNumber");
    Obj layer = send_obj0(marker, S("layer"));
    if (animation_class && number_class && layer) {
        Obj animation = send_obj1(animation_class, S("animationWithKeyPath:"),
                                  string_from_utf8("opacity"));
        Obj from = send_obj_double(number_class, S("numberWithDouble:"), 1.0);
        Obj to = send_obj_double(number_class, S("numberWithDouble:"), 0.0);
        if (animation && from && to) {
            send_void_obj(animation, S("setFromValue:"), from);
            send_void_obj(animation, S("setToValue:"), to);
            send_void_double(animation, S("setDuration:"), 0.45);
            send_void_obj(animation, S("setFillMode:"), string_from_utf8("forwards"));
            send_void_bool(animation, S("setRemovedOnCompletion:"), false);
            send_void_obj2(layer, S("addAnimation:forKey:"), animation,
                           string_from_utf8("mdc.tap.fade"));
        }
    }
    send_void_sel_obj_double(marker, S("performSelector:withObject:afterDelay:"),
                              S("mdc_removeTapMarker"), 0, 0.52);
}

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
    send_obj_double = msg;
    send_obj_rect = msg;
    send_obj_double4 = msg;
    send_point_to_space = msg;
    send_void_obj = msg;
    send_void_obj2 = msg;
    send_void_sel_obj_double = msg;
    send_void_bool = msg;
    send_void_double = msg;
    send_void_rect = msg;
    send_void_long = msg;
    send_void0 = msg;
    send_rect0 = msg;
    send_int0 = msg;
    send_unsigned0 = msg;
    send_bool_obj = msg;
    class_add_method = dlsym(objc, "class_addMethod");
    initialized = 1;
    return 0;
}

typedef struct { double x, y, width, height, normalized_x, normalized_y; int converted; } NormalizePointCtx;

static void normalize_point_on_main(void *p) {
    NormalizePointCtx *ctx = (NormalizePointCtx *)p;
    Obj screen_class = get_class("UIScreen");
    Obj screen = screen_class ? send_obj0(screen_class, S("mainScreen")) : 0;
    Obj current_space = screen ? send_obj0(screen, S("coordinateSpace")) : 0;
    Obj fixed_space = screen ? send_obj0(screen, S("fixedCoordinateSpace")) : 0;
    if (!current_space || !fixed_space || !send_point_to_space) return;

    MdcRect current_bounds = send_rect0(current_space, S("bounds"));
    MdcRect fixed_bounds = send_rect0(fixed_space, S("bounds"));
    if (current_bounds.size.width <= 0 || current_bounds.size.height <= 0 ||
        fixed_bounds.size.width <= 0 || fixed_bounds.size.height <= 0) return;

    MdcPoint current_point = {
        ctx->x * current_bounds.size.width / ctx->width,
        ctx->y * current_bounds.size.height / ctx->height
    };
    MdcPoint fixed_point = send_point_to_space(current_space,
        S("convertPoint:toCoordinateSpace:"), current_point, fixed_space);
    ctx->normalized_x = fixed_point.x / fixed_bounds.size.width;
    ctx->normalized_y = fixed_point.y / fixed_bounds.size.height;
    ctx->converted = 1;
}

int mdc_normalize_hid_point(double x, double y, double width, double height,
                            double *normalized_x, double *normalized_y) {
    if (!normalized_x || !normalized_y || width <= 0 || height <= 0) return -1;
    if (init_runtime() != 0) return -1;
    NormalizePointCtx ctx = { x, y, width, height, 0, 0, 0 };
    if (mdc_run_on_main_thread_sync(normalize_point_on_main, &ctx) != 0 || !ctx.converted) return -1;
    *normalized_x = ctx.normalized_x;
    *normalized_y = ctx.normalized_y;
    return 0;
}

typedef struct { double x, y, width, height; int shown; } TapCtx;

/* Create the shared, non-interactive overlay window on the active scene once;
 * used by both tap markers and toasts. Returns 1 when the overlay root view
 * is ready. */
static int ensure_overlay_window(void) {
    if (overlay_window && overlay_root) return 1;
    Obj screen_class = get_class("UIScreen");
    Obj window_class = get_class("UIWindow");
    Obj controller_class = get_class("UIViewController");
    Obj view_class = get_class("UIView");
    Obj color_class = get_class("UIColor");
    if (!screen_class || !window_class || !controller_class || !view_class || !color_class) return 0;
    if (!cleanup_method_installed && class_add_method) {
        cleanup_method_installed = class_add_method(view_class, S("mdc_removeTapMarker"),
                                                    marker_cleanup, "v@:");
    }
    if (!cleanup_method_installed) return 0;
    if (!status_method_installed && class_add_method) {
        status_method_installed = class_add_method(view_class, S("mdc_statusHideIf:"),
                                                   status_hide_if, "v@:@");
    }

    Obj screen = send_obj0(screen_class, S("mainScreen"));
    if (!screen) return 0;
    MdcRect bounds = send_rect0(screen, S("bounds"));
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return 0;

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
    if (!overlay_window) return 0;
    Obj clear = send_obj0(color_class, S("clearColor"));
    Obj controller = send_obj0(controller_class, S("alloc"));
    controller = send_obj0(controller, S("init"));
    if (!controller) return 0;
    send_void_obj(overlay_window, S("setRootViewController:"), controller);
    overlay_root = send_obj0(controller, S("view"));
    send_void0(controller, S("release"));
    if (!overlay_root) return 0;
    send_void_obj(overlay_window, S("setBackgroundColor:"), clear);
    send_void_bool(overlay_window, S("setOpaque:"), false);
    send_void_bool(overlay_window, S("setUserInteractionEnabled:"), false);
    send_void_double(overlay_window, S("setWindowLevel:"), 10000.0);
    send_void_obj(overlay_root, S("setBackgroundColor:"), clear);
    send_void_bool(overlay_root, S("setUserInteractionEnabled:"), false);
    send_void_bool(overlay_root, S("setAccessibilityElementsHidden:"), true);
    send_void_bool(overlay_window, S("setHidden:"), false);
    return 1;
}

static void show_on_main(void *p) {
    TapCtx *tap = (TapCtx *)p;
    Obj screen_class = get_class("UIScreen");
    Obj window_class = get_class("UIWindow");
    Obj controller_class = get_class("UIViewController");
    Obj view_class = get_class("UIView");
    Obj color_class = get_class("UIColor");
    if (!screen_class || !window_class || !controller_class || !view_class || !color_class) return;
    if (!overlay_window || !overlay_root) {
        if (!ensure_overlay_window()) return;
    }

    Obj screen = send_obj0(screen_class, S("mainScreen"));
    if (!screen) return;
    MdcRect bounds = send_rect0(screen, S("bounds"));
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return;

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
    fade_and_schedule_removal(marker);
    tap->shown = 1;
}

int mdc_show_tap(double x, double y, double width, double height) {
    if (init_runtime() != 0 || width <= 0 || height <= 0) return 0;
    TapCtx tap = { x, y, width, height, 0 };
    if (mdc_run_on_main_thread_sync(show_on_main, &tap) != 0) return 0;
    return tap.shown;
}

typedef struct { char text[MDC_MAX_TEXT + 1]; int shown; } ToastCtx;

/* Build the dark pill with a centered wrapping label. Returns a retained pill
 * view (the label is consumed) or NULL. */
static Obj make_pill(const char *text) {
    Obj label_class = get_class("UILabel");
    Obj font_class = get_class("UIFont");
    Obj view_class = get_class("UIView");
    Obj color_class = get_class("UIColor");
    if (!label_class || !font_class || !view_class || !color_class) return 0;
    Obj screen = send_obj0(get_class("UIScreen"), S("mainScreen"));
    if (!screen) return 0;
    MdcRect bounds = send_rect0(screen, S("bounds"));
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return 0;

    Obj label = send_obj0(label_class, S("alloc"));
    MdcRect label_frame = {{0, 0}, {bounds.size.width - 64.0, 40.0}};
    label = send_obj_rect(label, S("initWithFrame:"), label_frame);
    if (!label) return 0;
    send_void_bool(label, S("setUserInteractionEnabled:"), false);
    send_void_bool(label, S("setAccessibilityElementsHidden:"), true);
    send_void_obj(label, S("setText:"), string_from_utf8(text));
    Obj font = send_obj_double(font_class, S("boldSystemFontOfSize:"), 15.0);
    if (font) send_void_obj(label, S("setFont:"), font);
    Obj white = send_obj0(color_class, S("whiteColor"));
    if (white) send_void_obj(label, S("setTextColor:"), white);
    send_void_long(label, S("setTextAlignment:"), 1);
    send_void_long(label, S("setNumberOfLines:"), 0);
    send_void0(label, S("sizeToFit"));
    MdcRect fitted = send_rect0(label, S("frame"));
    if (fitted.size.width <= 0 || fitted.size.height <= 0) {
        send_void0(label, S("release"));
        return 0;
    }

    double pill_w = fitted.size.width + 28.0;
    double pill_h = fitted.size.height + 16.0;
    if (pill_w > bounds.size.width - 24.0) pill_w = bounds.size.width - 24.0;
    MdcRect pill_frame = {{(bounds.size.width - pill_w) / 2.0, 18.0}, {pill_w, pill_h}};
    Obj pill = send_obj0(view_class, S("alloc"));
    pill = send_obj_rect(pill, S("initWithFrame:"), pill_frame);
    if (!pill) {
        send_void0(label, S("release"));
        return 0;
    }
    send_void_bool(pill, S("setUserInteractionEnabled:"), false);
    send_void_bool(pill, S("setAccessibilityElementsHidden:"), true);
    Obj background = send_obj_double4(color_class, S("colorWithRed:green:blue:alpha:"),
                                      0.08, 0.08, 0.10, 0.94);
    send_void_obj(pill, S("setBackgroundColor:"), background);
    Obj layer = send_obj0(pill, S("layer"));
    send_void_double(layer, S("setCornerRadius:"), pill_h / 2.0);
    MdcRect inner = {{14.0, 8.0}, {fitted.size.width, fitted.size.height}};
    send_void_rect(label, S("setFrame:"), inner);
    send_void_obj(pill, S("addSubview:"), label);
    send_void0(label, S("release"));
    return pill;
}

static void toast_on_main(void *p) {
    ToastCtx *toast = (ToastCtx *)p;
    if (!overlay_window || !overlay_root) {
        if (!ensure_overlay_window()) return;
    }
    Obj pill = make_pill(toast->text);
    if (!pill) return;
    send_void_obj(overlay_root, S("addSubview:"), pill);
    // No release after addSubview: mdc_removeTapMarker (scheduled below) owns
    // the +1, matching the tap-marker lifecycle.
    send_void_sel_obj_double(pill, S("performSelector:withObject:afterDelay:"),
                             S("mdc_removeTapMarker"), 0, 2.4);
    toast->shown = 1;
}

int mdc_toast(const char *text) {
    if (!text || !text[0]) return 0;
    if (init_runtime() != 0) return 0;
    ToastCtx toast;
    memset(&toast, 0, sizeof(toast));
    snprintf(toast.text, sizeof(toast.text), "%s", text);
    if (mdc_run_on_main_thread_sync(toast_on_main, &toast) != 0) return 0;
    return toast.shown;
}

typedef struct { char text[MDC_MAX_TEXT + 1]; int shown; } StatusCtx;

static void status_show_on_main(void *p) {
    StatusCtx *status = (StatusCtx *)p;
    if (!overlay_window || !overlay_root) {
        if (!ensure_overlay_window()) return;
    }
    if (status_view) {
        send_void0(status_view, S("removeFromSuperview"));
        send_void0(status_view, S("release"));
        status_view = 0;
    }
    Obj pill = make_pill(status->text);
    if (!pill) return;
    send_void_obj(overlay_root, S("addSubview:"), pill);
    status_view = pill; /* status_view holds the +1 */
    status->shown = 1;
}

/* Persistent pill: stays until replaced with another mdc_status_show or
 * auto-hidden via mdc_status_autohide. */
int mdc_status_show(const char *text) {
    if (!text || !text[0]) return 0;
    if (init_runtime() != 0) return 0;
    StatusCtx status;
    memset(&status, 0, sizeof(status));
    snprintf(status.text, sizeof(status.text), "%s", text);
    if (mdc_run_on_main_thread_sync(status_show_on_main, &status) != 0) return 0;
    return status.shown;
}

static void status_autohide_on_main(void *p) {
    double seconds = *(double *)p;
    if (!status_view) return;
    send_void_sel_obj_double(status_view, S("performSelector:withObject:afterDelay:"),
                             S("mdc_statusHideIf:"), status_view, seconds);
}

int mdc_status_autohide(double seconds) {
    if (init_runtime() != 0) return 0;
    if (seconds <= 0) seconds = 2.0;
    double boxed = seconds;
    if (mdc_run_on_main_thread_sync(status_autohide_on_main, &boxed) != 0) return 0;
    return 1;
}
