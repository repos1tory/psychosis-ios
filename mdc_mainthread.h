#ifndef MDC_MAINTHREAD_H
#define MDC_MAINTHREAD_H

typedef void (*MdcMainThreadTask)(void *context);

/* Run a short UIKit/Objective-C task on the process main thread and wait for it. */
int mdc_run_on_main_thread_sync(MdcMainThreadTask task, void *context);

#endif
