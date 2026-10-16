#ifndef MDC_OVERLAY_H
#define MDC_OVERLAY_H

int mdc_show_tap(double x, double y, double width, double height);
int mdc_normalize_hid_point(double x, double y, double width, double height,
                            double *normalized_x, double *normalized_y);

/* Non-interactive status pill at the top of the screen (auto-fades). */
int mdc_toast(const char *text);

/* Persistent status pill: shown until replaced or auto-hidden. */
int mdc_status_show(const char *text);
int mdc_status_autohide(double seconds);

#endif
