// mdtouch - system-wide touch/key injection for jailbroken iOS.
// Pure C, no SDK headers: resolves everything via dlopen/dlsym.
// Construction follows Veency's VNCPointerNew (Hand + Finger child events).
// Usage:
//   mdtouch tap X Y [W H]          (pixels, default 768x1024 portrait)
//   mdtouch swipe X1 Y1 X2 Y2 [steps]
//   mdtouch home
// Must run as root: sudo ./mdtouch tap 384 512
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *CFRef;
typedef double IOHIDFloat; // LP64
typedef int Boolean;

// resolved symbols
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
static void (*P_SetSender)(CFRef, uint64_t);
static uint64_t (*P_AbsTime)(void);
static void (*P_CFRelease)(CFRef);

static CFRef client = 0;

// exact values from Apple IOHIDFamily IOHIDEventTypes.h
#define T_HAND 3
#define EV_RANGE 1
#define EV_TOUCH 2
#define EV_POS 4
#define EV_ID 32
#define F_BUILTIN 4
#define F_DISP_INT ((11u << 16) + 25) // 720921
#define SENDER 0xDEFACEDBEEFFECE5ull
#define HID_PAGE_CONSUMER 0x0C
#define HID_USE_MENU 0x40

static int resolve(void) {
    void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (!h) { fprintf(stderr, "IOKit dlopen fail: %s\n", dlerror()); return -1; }
    P_CreateClient = dlsym(h, "IOHIDEventSystemClientCreate");
    P_Dispatch = dlsym(h, "IOHIDEventSystemClientDispatchEvent");
    P_SetMatching = dlsym(h, "IOHIDEventSystemClientSetMatching");
    P_CreateHand = dlsym(h, "IOHIDEventCreateDigitizerEvent");
    P_CreateFinger = dlsym(h, "IOHIDEventCreateDigitizerFingerEvent");
    P_CreateKey = dlsym(h, "IOHIDEventCreateKeyboardEvent");
    P_Append = dlsym(h, "IOHIDEventAppendEvent");
    P_SetInt = dlsym(h, "IOHIDEventSetIntegerValue");
    P_SetSender = dlsym(h, "IOHIDEventSetSenderID");
    void *sys = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY);
    P_AbsTime = dlsym(sys, "mach_absolute_time");
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
    P_CFRelease = dlsym(cf, "CFRelease");
    if (!P_CreateClient || !P_Dispatch || !P_SetMatching || !P_CreateHand || !P_CreateFinger ||
        !P_Append || !P_SetInt || !P_SetSender || !P_AbsTime || !P_CFRelease) {
        fprintf(stderr, "symbol resolve fail\n");
        return -1;
    }
    client = P_CreateClient(0);
    if (!client) { fprintf(stderr, "HID client NULL (need root?)\n"); return -1; }
    P_SetMatching(client, 0); // subscribe to all HID services, else dispatch goes nowhere
    return 0;
}

// phase: 0=down 1=move 2=up ; nx,ny normalized 0..1
static void send_phase(int phase, double nx, double ny) {
    uint32_t handm, fingerm;
    Boolean touching;
    if (phase == 0) { handm = EV_RANGE | EV_TOUCH | EV_ID; fingerm = EV_RANGE | EV_TOUCH; touching = 1; }
    else if (phase == 1) { handm = EV_POS; fingerm = EV_POS; touching = 1; }
    else { handm = EV_RANGE | EV_TOUCH | EV_ID | EV_POS; fingerm = EV_RANGE | EV_TOUCH; touching = 0; }
    CFRef hand = P_CreateHand(0, P_AbsTime(), T_HAND, (1u << 22), 1, handm, 0,
                              nx, ny, 0, 0, 0, 0, 0, 0);
    P_SetInt(hand, F_BUILTIN, 1);
    P_SetInt(hand, F_DISP_INT, 1);
    CFRef fg = P_CreateFinger(0, P_AbsTime(), 3, 2, fingerm,
                              nx, ny, 0, 0, 0, touching, touching, 0);
    P_Append(hand, fg);
    P_CFRelease(fg);
    P_SetSender(hand, SENDER);
    P_Dispatch(client, hand);
    P_CFRelease(hand);
}

static void msleep(int ms) { usleep((useconds_t)ms * 1000); }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: mdtouch tap X Y [W H] | swipe X1 Y1 X2 Y2 [steps] | home\n");
        return 1;
    }
    if (resolve() != 0) return 1;
    if (!strcmp(argv[1], "tap") && argc >= 4) {
        double w = argc >= 6 ? atof(argv[4]) : 768.0;
        double h = argc >= 6 ? atof(argv[5]) : 1024.0;
        double nx = atof(argv[2]) / w, ny = atof(argv[3]) / h;
        send_phase(0, nx, ny); msleep(60); send_phase(2, nx, ny);
        printf("tap %s,%s ok\n", argv[2], argv[3]);
    } else if (!strcmp(argv[1], "swipe") && argc >= 6) {
        double w = 768.0, h = 1024.0;
        double x1 = atof(argv[2]) / w, y1 = atof(argv[3]) / h;
        double x2 = atof(argv[4]) / w, y2 = atof(argv[5]) / h;
        int steps = argc >= 7 ? atoi(argv[6]) : 20;
        send_phase(0, x1, y1); msleep(20);
        for (int i = 1; i <= steps; i++) {
            send_phase(1, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
            msleep(15);
        }
        send_phase(2, x2, y2);
        printf("swipe ok\n");
    } else if (!strcmp(argv[1], "home") || !strcmp(argv[1], "volup")) {
        if (!P_CreateKey) { fprintf(stderr, "no key API\n"); return 1; }
        uint16_t usage = !strcmp(argv[1], "home") ? HID_USE_MENU : 0xE9;
        CFRef d = P_CreateKey(0, P_AbsTime(), HID_PAGE_CONSUMER, usage, 1, 0);
        P_SetSender(d, SENDER); P_Dispatch(client, d); P_CFRelease(d);
        msleep(100);
        CFRef u = P_CreateKey(0, P_AbsTime(), HID_PAGE_CONSUMER, usage, 0, 0);
        P_SetSender(u, SENDER); P_Dispatch(client, u); P_CFRelease(u);
        printf("%s ok\n", argv[1]);
    } else { fprintf(stderr, "bad args\n"); return 1; }
    P_CFRelease(client);
    return 0;
}
