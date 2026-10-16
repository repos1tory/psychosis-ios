#ifndef MDC_SERVICES_H
#define MDC_SERVICES_H

#include <stddef.h>

/* Post a user-visible notification (banner) with the given body text.
 * Returns 0 on success, -1 when the UserNotifications path is unavailable. */
int mdc_notify(const char *text);

/* Capture the full display to a PNG at `path_io` (defaults to
 * /tmp/psychosis-shot.png when empty). On success returns 0, writes the final
 * path back into `path_io`, and fills pixel dimensions. */
int mdc_shot(char *path_io, size_t cap, int *out_w, int *out_h);

/* Logical screen size in points (for tap redirects by label). */
int mdc_screen_size(double *out_w, double *out_h);

/* Text-to-speech with temporary volume: saves the current active-category
 * volume, raises it to target_volume (0..1; <=0 means default 0.7), speaks,
 * waits for completion, then restores the saved volume. `voice` optionally
 * selects a voice by name/identifier substring (NULL = best installed).
 * `interruptible` stops the speech as soon as the mic hears the user.
 * Returns 0 ok, -3 interrupted by the user, -2 spoken but completion unknown,
 * -1 unavailable. */
int mdc_say(const char *text, double target_volume, const char *voice, int interruptible);

/* Writes a "ok voices\\n" listing of installed voices (name, language,
 * quality, identifier). Returns 0 on success. */
int mdc_say_voices(char *out, size_t cap);

/* ------------------------------------------------------------------ */
/* record - microphone capture to a WAV file                           */
/* ------------------------------------------------------------------ */
int mdc_record(double seconds, char *path_io, size_t cap);

/* One-shot speech-triggered recording: blocks until the caller speaks and
 * then goes quiet for silence_ms, or until timeout_s pass with no speech.
 * Writes a WAV to path_io and reports the spoken duration. Status pill shows
 * listening / recording / transcribing states.
 * Returns 0 ok, -4 when no speech was detected in time. */
int mdc_listen_turn(double timeout_s, double silence_ms, double min_speech_ms,
                    char *path_io, size_t cap, double *out_seconds);

/* System pasteboard. get returns 0 and copies the string into out (may be
 * empty); set writes text. Reads run on a worker thread, and cross-app
 * content falls back to pasteboardd's on-disk store; -2 only when both
 * paths fail. */
int mdc_clipboard_get(char *out, size_t cap);
int mdc_clipboard_set(const char *text);

/* Open a URL / scheme from SpringBoard (main thread internally). */
int mdc_open_url(const char *url);

/* Bounds of UIKit's fixedCoordinateSpace (screen space that AX frames use,
 * independent of interface orientation). */
int mdc_fixed_screen_size(double *out_w, double *out_h);

/* Convert a fixed-space point (AX frame coordinates) to the current
 * interface-orientation space that tap/swipe consume. */
int mdc_fixed_to_current(double x, double y, double *out_x, double *out_y);

#endif
