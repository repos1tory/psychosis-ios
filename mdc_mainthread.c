#include "mdc_mainthread.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>

typedef void *Obj;
typedef void *Sel;
typedef void *Class;
typedef void (*Imp)(void);

typedef Class (*GetClassFn)(const char *name);
typedef Sel (*RegisterSelectorFn)(const char *name);
typedef Class (*AllocateClassPairFn)(Class superclass, const char *name, unsigned long extraBytes);
typedef void (*RegisterClassPairFn)(Class cls);
typedef bool (*AddMethodFn)(Class cls, Sel selector, Imp implementation, const char *types);
typedef Obj (*SendObject0Fn)(Obj, Sel);
typedef Obj (*SendObject1Fn)(Obj, Sel, Obj);
typedef bool (*SendBool0Fn)(Obj, Sel);
typedef void (*SendVoid0Fn)(Obj, Sel);
typedef void (*PerformSelectorOnMainThreadFn)(Obj, Sel, Sel, Obj, bool);

typedef struct {
    MdcMainThreadTask task;
    void *context;
} MainTaskBox;

static void *g_objc;
static GetClassFn g_get_class;
static RegisterSelectorFn g_register_selector;
static AllocateClassPairFn g_allocate_class_pair;
static RegisterClassPairFn g_register_class_pair;
static AddMethodFn g_add_method;
static SendObject0Fn g_send_object0;
static SendObject1Fn g_send_object1;
static SendBool0Fn g_send_bool0;
static SendVoid0Fn g_send_void0;
static PerformSelectorOnMainThreadFn g_perform_on_main;
static Class g_task_host_class;
static Obj g_task_host;
static Sel g_run_task_selector;
static int g_init_result = -1;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static void run_boxed_task(Obj self, Sel command, Obj boxed) {
    (void)self;
    (void)command;
    if (!boxed || !g_send_object0) return;
    void *pointer = g_send_object0(boxed, g_register_selector("pointerValue"));
    MainTaskBox *box = (MainTaskBox *)pointer;
    if (box && box->task) box->task(box->context);
}

static void initialize_runtime(void) {
    g_objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_objc) g_objc = dlopen("libobjc.A.dylib", RTLD_LAZY | RTLD_GLOBAL);
    if (!g_objc) return;
    g_get_class = (GetClassFn)dlsym(g_objc, "objc_getClass");
    g_register_selector = (RegisterSelectorFn)dlsym(g_objc, "sel_registerName");
    g_allocate_class_pair = (AllocateClassPairFn)dlsym(g_objc, "objc_allocateClassPair");
    g_register_class_pair = (RegisterClassPairFn)dlsym(g_objc, "objc_registerClassPair");
    g_add_method = (AddMethodFn)dlsym(g_objc, "class_addMethod");
    void *message_send = dlsym(g_objc, "objc_msgSend");
    if (!g_get_class || !g_register_selector || !g_allocate_class_pair ||
        !g_register_class_pair || !g_add_method || !message_send) return;

    g_send_object0 = (SendObject0Fn)message_send;
    g_send_object1 = (SendObject1Fn)message_send;
    g_send_bool0 = (SendBool0Fn)message_send;
    g_send_void0 = (SendVoid0Fn)message_send;
    g_perform_on_main = (PerformSelectorOnMainThreadFn)message_send;
    Class nsobject = g_get_class("NSObject");
    if (!nsobject) return;
    g_task_host_class = g_get_class("MDCMainThreadTaskHost");
    if (!g_task_host_class) {
        g_task_host_class = g_allocate_class_pair(nsobject, "MDCMainThreadTaskHost", 0);
        if (!g_task_host_class) return;
        g_run_task_selector = g_register_selector("mdc_runTask:");
        if (!g_add_method(g_task_host_class, g_run_task_selector,
                          (Imp)run_boxed_task, "v@:@")) return;
        g_register_class_pair(g_task_host_class);
    } else {
        g_run_task_selector = g_register_selector("mdc_runTask:");
    }
    Obj allocated = g_send_object0((Obj)g_task_host_class, g_register_selector("alloc"));
    g_task_host = allocated ? g_send_object0(allocated, g_register_selector("init")) : 0;
    if (!g_task_host || !g_run_task_selector) return;
    g_init_result = 0;
}

int mdc_run_on_main_thread_sync(MdcMainThreadTask task, void *context) {
    if (!task) return -1;
    pthread_once(&g_init_once, initialize_runtime);
    if (g_init_result != 0) return -1;

    Obj thread_class = (Obj)g_get_class("NSThread");
    if (!thread_class) return -1;
    if (g_send_bool0(thread_class, g_register_selector("isMainThread"))) {
        task(context);
        return 0;
    }

    Class pool_class = g_get_class("NSAutoreleasePool");
    Obj pool = pool_class ? g_send_object0((Obj)pool_class, g_register_selector("alloc")) : 0;
    if (pool) pool = g_send_object0(pool, g_register_selector("init"));
    if (!pool) return -1;
    Class value_class = g_get_class("NSValue");
    if (!value_class) {
        g_send_void0(pool, g_register_selector("drain"));
        return -1;
    }
    Obj value = g_send_object1((Obj)value_class, g_register_selector("valueWithPointer:"),
                               (Obj)(&(MainTaskBox){task, context}));
    if (!value) {
        g_send_void0(pool, g_register_selector("drain"));
        return -1;
    }
    g_perform_on_main(g_task_host,
                      g_register_selector("performSelectorOnMainThread:withObject:waitUntilDone:"),
                      g_run_task_selector, value, true);
    /* valueWithPointer: stores the pointer, not its contents; the stack box
       must remain live until performSelectorOnMainThread returns. */
    g_send_void0(pool, g_register_selector("drain"));
    return 0;
}
