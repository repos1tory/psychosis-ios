// mdc_services.c - notify (UserNotifications) and shot (display capture) for
// the Psychosis/MDControl SpringBoard dylib. All framework access is dynamic
// (dlopen + dlsym + raw Objective-C runtime) so nothing needs to be linked.
#include "mdc_services.h"
#include "mdc_mainthread.h"
#include "mdc_protocol.h"

#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mdc_overlay.h"

static void shot_log(const char *stage, long value) {
    int fd = open("/var/mobile/Library/Logs/Psychosis-shot.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    char line[96];
    int n = snprintf(line, sizeof(line), "%s %ld\n", stage, value);
    if (n > 0) write(fd, line, (size_t)n);
    close(fd);
}

typedef void *OID;
typedef void *OSEL;
typedef void *CFRef;
typedef struct { double x, y, w, h; } R64;

static void *g_msg_send;
static void *g_objc_handle;

static OID (*rt_get_class)(const char *);
static OSEL (*rt_sel)(const char *);
static OID (*rt_msg0)(OID, OSEL);
static OID (*rt_msg1)(OID, OSEL, OID);
static OID (*rt_msg1d)(OID, OSEL, double);
static void (*rt_void1)(OID, OSEL, OID);
static void (*rt_void2)(OID, OSEL, OID, OID);
static OID (*rt_msg3)(OID, OSEL, OID, OID, OID);

static int mdc_rt_init(void) {
    if (g_msg_send) return 0;
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_GLOBAL);
    if (!objc) return -1;
    g_objc_handle = objc;
    rt_get_class = (OID (*)(const char *))dlsym(objc, "objc_getClass");
    rt_sel = (OSEL (*)(const char *))dlsym(objc, "sel_registerName");
    g_msg_send = dlsym(objc, "objc_msgSend");
    if (!rt_get_class || !rt_sel || !g_msg_send) return -1;
    rt_msg0 = (OID (*)(OID, OSEL))g_msg_send;
    rt_msg1 = (OID (*)(OID, OSEL, OID))g_msg_send;
    rt_msg1d = (OID (*)(OID, OSEL, double))g_msg_send;
    rt_void1 = (void (*)(OID, OSEL, OID))g_msg_send;
    rt_void2 = (void (*)(OID, OSEL, OID, OID))g_msg_send;
    rt_msg3 = (OID (*)(OID, OSEL, OID, OID, OID))g_msg_send;
    return 0;
}

static OID mdc_str(const char *utf8) {
    OID cls = rt_get_class("NSString");
    return cls ? rt_msg1(cls, rt_sel("stringWithUTF8String:"), (OID)utf8) : 0;
}

/* ------------------------------------------------------------------ */
/* notify                                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* notify                                                              */
/* ------------------------------------------------------------------ */

static void notify_log(const char *stage, long value) {
    int fd = open("/var/mobile/Library/Logs/Psychosis-notify.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    char line[96];
    int n = snprintf(line, sizeof(line), "%s %ld\n", stage, value);
    if (n > 0) write(fd, line, (size_t)n);
    close(fd);
}

/* SpringBoard is effectively always the foreground "app"; without a delegate
 * returning presentation options, iOS suppresses the banner. */
static void notify_will_present(OID self, OSEL sel, OID center, OID notification, void *handler) {
    (void)self; (void)sel; (void)center; (void)notification;
    notify_log("will-present", 1);
    if (handler) ((void (^)(unsigned long))handler)(0x1F /* badge|sound|alert|list|banner */);
}

typedef struct { char text[MDC_MAX_TEXT + 1]; volatile int toast_fired; } NotifyFailCtx;

static void notify_fail_toast(NotifyFailCtx *ctx) {
    if (__sync_bool_compare_and_swap(&ctx->toast_fired, 0, 1)) {
        /* ctx is intentionally leaked: it is tiny, notify is rare, and both
         * completion blocks may race to read it. */
        mdc_toast(ctx->text);
    }
}

static int ensure_notify_delegate(OID center) {
    static int installed = 0;
    static OID delegate = 0;
    if (installed) return 0;
    OID (*allocate_pair)(OID, const char *, unsigned long) =
        dlsym(g_objc_handle, "objc_allocateClassPair");
    void (*register_pair)(OID) = dlsym(g_objc_handle, "objc_registerClassPair");
    bool (*add_method)(OID, OSEL, void (*)(void), const char *) =
        dlsym(g_objc_handle, "class_addMethod");
    if (!allocate_pair || !register_pair || !add_method) return -1;
    OID cls = allocate_pair(rt_get_class("NSObject"), "PsychosisNotifyDelegate", 0);
    if (!cls) return -1;
    if (!add_method(cls, rt_sel("userNotificationCenter:willPresentNotification:withCompletionHandler:"),
                    (void (*)(void))notify_will_present, "v@:@@@?"))
        return -1;
    register_pair(cls);
    OID allocated = rt_msg0(cls, rt_sel("alloc"));
    delegate = allocated ? rt_msg0(allocated, rt_sel("init")) : 0;
    if (!delegate) return -1;
    rt_void1(center, rt_sel("setDelegate:"), delegate);
    installed = 1;
    notify_log("delegate", 1);
    return 0;
}

int mdc_notify(const char *text) {
    if (!text || !text[0]) return -1;
    void *h_un = dlopen("/System/Library/Frameworks/UserNotifications.framework/UserNotifications",
                        RTLD_LAZY);
    if (!h_un) return -1;
    if (mdc_rt_init() != 0) return -1;

    OID content_class = rt_get_class("UNMutableNotificationContent");
    OID request_class = rt_get_class("UNNotificationRequest");
    OID center_class = rt_get_class("UNUserNotificationCenter");
    if (!content_class || !request_class || !center_class) return -1;

    OID allocated = rt_msg0(content_class, rt_sel("alloc"));
    OID content = allocated ? rt_msg0(allocated, rt_sel("init")) : 0;
    OID title = mdc_str("Psychosis");
    OID body = mdc_str(text);
    OID title_key = mdc_str("title");
    OID body_key = mdc_str("body");
    if (!content || !title || !body || !title_key || !body_key) return -1;
    rt_void2(content, rt_sel("setValue:forKey:"), title, title_key);
    rt_void2(content, rt_sel("setValue:forKey:"), body, body_key);
    OID sound_class = rt_get_class("UNNotificationSound");
    OID sound = sound_class ? rt_msg0(sound_class, rt_sel("defaultSound")) : 0;
    OID sound_key = mdc_str("sound");
    if (sound && sound_key) rt_void2(content, rt_sel("setValue:forKey:"), sound, sound_key);

    OID identifier = mdc_str("psychosis");
    OID request = identifier
        ? rt_msg3(request_class, rt_sel("requestWithIdentifier:content:trigger:"),
                  identifier, content, 0)
        : 0;
    OID center = rt_msg0(center_class, rt_sel("currentNotificationCenter"));
    if (!request || !center) return -1;
    ensure_notify_delegate(center);

    NotifyFailCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    snprintf(ctx->text, sizeof(ctx->text), "%s", text);

    void (^auth_handler)(bool, OID) = ^(bool granted, OID error) {
        (void)error;
        notify_log("auth", granted ? 1 : 0);
        if (!granted) notify_fail_toast(ctx);
    };
    void (^add_handler)(OID) = ^(OID error) {
        notify_log("add", error ? -1 : 0);
        if (error) notify_fail_toast(ctx);
    };
    typedef void (*AuthSend)(OID, OSEL, unsigned long, void *);
    typedef void (*AddSend)(OID, OSEL, OID, void *);
    ((AuthSend)g_msg_send)(center, rt_sel("requestAuthorizationWithOptions:completionHandler:"),
                           6 /* alert|sound */, (void *)auth_handler);
    ((AddSend)g_msg_send)(center, rt_sel("addNotificationRequest:withCompletionHandler:"),
                          request, (void *)add_handler);
    return 0;
}

/* ------------------------------------------------------------------ */
/* say - text-to-speech with temporary volume (AVSpeechSynthesizer +   */
/* the private AVSystemController volume API)                          */
/* ------------------------------------------------------------------ */

static pthread_mutex_t g_say_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_say_cond = PTHREAD_COND_INITIALIZER;
static int g_say_done;
static OID g_say_synth;
static OID g_say_delegate;
static int g_say_ready;

static void say_finished(OID self, OSEL sel, OID synth, OID utterance) {
    (void)self; (void)sel; (void)synth; (void)utterance;
    pthread_mutex_lock(&g_say_lock);
    g_say_done = 1;
    pthread_cond_signal(&g_say_cond);
    pthread_mutex_unlock(&g_say_lock);
}

static int say_ensure(void) {
    if (g_say_ready) return g_say_ready < 0 ? -1 : 0;
    g_say_ready = -1;
    if (mdc_rt_init() != 0) return -1;
    void *av = dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_LAZY);
    if (!av) return -1;
    OID synth_class = rt_get_class("AVSpeechSynthesizer");
    OID utterance_class = rt_get_class("AVSpeechUtterance");
    if (!synth_class || !utterance_class) return -1;
    OID (*allocate_pair)(OID, const char *, unsigned long) =
        dlsym(g_objc_handle, "objc_allocateClassPair");
    void (*register_pair)(OID) = dlsym(g_objc_handle, "objc_registerClassPair");
    bool (*add_method)(OID, OSEL, void (*)(void), const char *) =
        dlsym(g_objc_handle, "class_addMethod");
    if (!allocate_pair || !register_pair || !add_method) return -1;
    OID cls = allocate_pair(rt_get_class("NSObject"), "PsychosisSayDelegate", 0);
    if (!cls) return -1;
    if (!add_method(cls, rt_sel("speechSynthesizer:didFinishSpeechUtterance:"),
                    (void (*)(void))say_finished, "v@:@@"))
        return -1;
    if (!add_method(cls, rt_sel("speechSynthesizer:didCancelSpeechUtterance:"),
                    (void (*)(void))say_finished, "v@:@@"))
        return -1;
    register_pair(cls);
    OID allocated = rt_msg0(cls, rt_sel("alloc"));
    g_say_delegate = allocated ? rt_msg0(allocated, rt_sel("init")) : 0;
    if (!g_say_delegate) return -1;
    allocated = rt_msg0(synth_class, rt_sel("alloc"));
    g_say_synth = allocated ? rt_msg0(allocated, rt_sel("init")) : 0;
    if (!g_say_synth) return -1;
    rt_void1(g_say_synth, rt_sel("setDelegate:"), g_say_delegate);
    g_say_ready = 1;
    return 0;
}

/* Pick a voice: explicit substring match on name/identifier first, otherwise
 * the best-quality installed English voice (premium > enhanced > default). */
static OID say_pick_voice(const char *want) {
    OID voice_class = rt_get_class("AVSpeechSynthesisVoice");
    if (!voice_class) return 0;
    signed char (*responds)(OID, OSEL, OSEL) = (signed char (*)(OID, OSEL, OSEL))g_msg_send;
    OSEL list_sel = rt_sel("speechVoices");
    if (!responds(voice_class, rt_sel("respondsToSelector:"), list_sel)) return 0;
    OID list = ((OID (*)(OID, OSEL))g_msg_send)(voice_class, list_sel);
    if (!list) return 0;
    unsigned long count = ((unsigned long (*)(OID, OSEL))g_msg_send)(list, rt_sel("count"));
    OID best = 0;
    long best_score = -1;
    for (unsigned long i = 0; i < count; i++) {
        OID voice = ((OID (*)(OID, OSEL, unsigned long))g_msg_send)(
            list, rt_sel("objectAtIndex:"), i);
        if (!voice) continue;
        OID name_obj = rt_msg0(voice, rt_sel("name"));
        OID ident_obj = rt_msg0(voice, rt_sel("identifier"));
        OID lang_obj = rt_msg0(voice, rt_sel("language"));
        const char *name = name_obj
            ? ((const char *(*)(OID, OSEL))g_msg_send)(name_obj, rt_sel("UTF8String")) : 0;
        const char *ident = ident_obj
            ? ((const char *(*)(OID, OSEL))g_msg_send)(ident_obj, rt_sel("UTF8String")) : 0;
        const char *lang = lang_obj
            ? ((const char *(*)(OID, OSEL))g_msg_send)(lang_obj, rt_sel("UTF8String")) : 0;
        long quality = ((long (*)(OID, OSEL))g_msg_send)(voice, rt_sel("quality"));
        if (want && want[0]) {
            int name_hit = name && strcasestr(name, want);
            int ident_hit = ident && strcasestr(ident, want);
            if (name_hit || ident_hit) {
                long score = 1000 + quality;
                if (score > best_score) {
                    best = voice;
                    best_score = score;
                }
            }
            continue;
        }
        if (!lang || strncmp(lang, "en", 2) != 0) continue;
        /* quality first, then Siri compact, then standard compact voices;
         * keep novelty/eloquence voices out of the automatic pick. */
        long score = quality * 100;
        if (ident && strstr(ident, "siri")) score += 60;
        if (ident && strstr(ident, "voice.compact")) score += 40;
        if (ident && (strstr(ident, "eloquence") || strstr(ident, "speech.synthesis.voice")))
            score -= 500;
        if (strncmp(lang, "en-US", 5) == 0) score += 10;
        else score += 5;
        if (score > best_score) {
            best = voice;
            best_score = score;
        }
    }
    return best;
}

int mdc_say_voices(char *out, size_t cap) {
    if (!out || cap < 32) return -1;
    if (say_ensure() != 0) return -1;
    OID voice_class = rt_get_class("AVSpeechSynthesisVoice");
    OID list = voice_class
        ? ((OID (*)(OID, OSEL))g_msg_send)(voice_class, rt_sel("speechVoices")) : 0;
    if (!list) return -1;
    unsigned long count = ((unsigned long (*)(OID, OSEL))g_msg_send)(list, rt_sel("count"));
    size_t used = (size_t)snprintf(out, cap, "ok voices %lu\n", count);
    for (unsigned long i = 0; i < count && used + 96 < cap; i++) {
        OID voice = ((OID (*)(OID, OSEL, unsigned long))g_msg_send)(
            list, rt_sel("objectAtIndex:"), i);
        if (!voice) continue;
        OID name_obj = rt_msg0(voice, rt_sel("name"));
        OID ident_obj = rt_msg0(voice, rt_sel("identifier"));
        OID lang_obj = rt_msg0(voice, rt_sel("language"));
        const char *name = name_obj
            ? ((const char *(*)(OID, OSEL))g_msg_send)(name_obj, rt_sel("UTF8String")) : "?";
        const char *ident = ident_obj
            ? ((const char *(*)(OID, OSEL))g_msg_send)(ident_obj, rt_sel("UTF8String")) : "?";
        const char *lang = lang_obj
            ? ((const char *(*)(OID, OSEL))g_msg_send)(lang_obj, rt_sel("UTF8String")) : "?";
        long quality = ((long (*)(OID, OSEL))g_msg_send)(voice, rt_sel("quality"));
        const char *qname = quality >= 3 ? "premium" : (quality == 2 ? "enhanced" : "default");
        used += (size_t)snprintf(out + used, cap - used, "%s | %s | %s | %s\n",
                                 name, lang, qname, ident);
    }
    return 0;
}

static OID say_volume_controller(void) {
    OID cls = rt_get_class("AVSystemController");
    if (!cls) {
        dlopen("/System/Library/PrivateFrameworks/MediaPlayer.framework/MediaPlayer", RTLD_LAZY);
        cls = rt_get_class("AVSystemController");
    }
    if (!cls) return 0;
    signed char (*responds)(OID, OSEL, OSEL) = (signed char (*)(OID, OSEL, OSEL))g_msg_send;
    OSEL sel = rt_sel("sharedAVSystemController");
    if (!responds(cls, rt_sel("respondsToSelector:"), sel)) return 0;
    return ((OID (*)(OID, OSEL))g_msg_send)(cls, sel);
}

/* Reads the active audio category volume; the category object is borrowed. */
static int say_get_volume(float *value, OID *category_out) {
    OID controller = say_volume_controller();
    if (!controller) return -1;
    signed char (*responds)(OID, OSEL, OSEL) = (signed char (*)(OID, OSEL, OSEL))g_msg_send;
    OSEL sel = rt_sel("getActiveCategoryVolume:andName:");
    if (!responds(controller, rt_sel("respondsToSelector:"), sel)) return -1;
    float v = 0;
    OID name = 0;
    signed char ok = ((signed char (*)(OID, OSEL, float *, OID *))g_msg_send)(
        controller, sel, &v, &name);
    if (!ok || !name) return -1;
    if (value) *value = v;
    if (category_out) *category_out = name;
    return 0;
}

static int say_set_volume(float value, OID category) {
    OID controller = say_volume_controller();
    if (!controller || !category) return -1;
    signed char (*responds)(OID, OSEL, OSEL) = (signed char (*)(OID, OSEL, OSEL))g_msg_send;
    OSEL sel = rt_sel("setVolumeTo:forCategory:error:");
    if (!responds(controller, rt_sel("respondsToSelector:"), sel)) return -1;
    signed char ok = ((signed char (*)(OID, OSEL, float, OID, OID *))g_msg_send)(
        controller, sel, value, category, 0);
    return ok ? 0 : -1;
}

    /* Route speech through the playback session so it is audible even when the
 * ring/silent switch is engaged (category NULL keeps the current setup). */
static void say_audio_session(const char *category_name, int active) {
    OID cls = rt_get_class("AVAudioSession");
    if (!cls) return;
    typedef signed char (*RespondsFn)(OID, OSEL, OSEL);
    RespondsFn rp = (RespondsFn)g_msg_send;
    OID (*responds0)(OID, OSEL, OSEL) = (OID (*)(OID, OSEL, OSEL))g_msg_send;
    (void)responds0;
    OSEL shared_sel = rt_sel("sharedInstance");
    if (!rp(cls, rt_sel("respondsToSelector:"), shared_sel)) return;
    OID session = ((OID (*)(OID, OSEL))g_msg_send)(cls, shared_sel);
    if (!session) return;
    if (active && category_name) {
        OSEL sel = rt_sel("setCategory:error:");
        if (rp(session, rt_sel("respondsToSelector:"), sel)) {
            OID category = mdc_str(category_name);
            if (category)
                ((signed char (*)(OID, OSEL, OID, OID *))g_msg_send)(session, sel, category, 0);
        }
    }
    OSEL active_sel = rt_sel("setActive:error:");
    if (rp(session, rt_sel("respondsToSelector:"), active_sel))
        ((signed char (*)(OID, OSEL, signed char, OID *))g_msg_send)(
            session, active_sel, active ? 1 : 0, 0);
}

/* Lowest dBFS level (metering) that counts as speech for VAD and barge-in. */
#define PSYCHOSIS_SPEECH_DB (-38.0f)
#define PSYCHOSIS_METER_POLL_MS 50

/* Builds a 16 kHz / 16-bit / mono WAV AVAudioRecorder aimed at `path`. */
static OID make_wav_recorder(const char *path) {
    OID recorder_class = rt_get_class("AVAudioRecorder");
    OID url_class = rt_get_class("NSURL");
    OID dict_class = rt_get_class("NSMutableDictionary");
    OID number_class = rt_get_class("NSNumber");
    if (!recorder_class || !url_class || !dict_class || !number_class) return 0;
    OID settings = rt_msg0(dict_class, rt_sel("dictionary"));
    if (!settings) return 0;
    struct { const char *key; long value; } ints[] = {
        {"AVFormatIDKey", 0x6C70636D},          /* kAudioFormatLinearPCM */
        {"AVNumberOfChannelsKey", 1},
        {"AVLinearPCMBitDepthKey", 16},
        {"AVLinearPCMIsFloatKey", 0},
        {"AVLinearPCMIsBigEndianKey", 0},
    };
    for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
        OID key = mdc_str(ints[i].key);
        OID value = key
            ? rt_msg1(number_class, rt_sel("numberWithInt:"), (OID)(intptr_t)ints[i].value)
            : 0;
        if (!key || !value) return 0;
        rt_void2(settings, rt_sel("setValue:forKey:"), value, key);
    }
    OID rate_key = mdc_str("AVSampleRateKey");
    OID rate_value = rate_key
        ? rt_msg1d(number_class, rt_sel("numberWithDouble:"), 16000.0) : 0;
    if (rate_key && rate_value)
        rt_void2(settings, rt_sel("setValue:forKey:"), rate_value, rate_key);
    OID ns_path = mdc_str(path);
    OID url = ns_path ? rt_msg1(url_class, rt_sel("fileURLWithPath:"), ns_path) : 0;
    if (!url) return 0;
    OID error = 0;
    OID allocated = rt_msg0(recorder_class, rt_sel("alloc"));
    return allocated
        ? ((OID (*)(OID, OSEL, OID, OID, OID *))g_msg_send)(
              allocated, rt_sel("initWithURL:settings:error:"), url, settings, &error)
        : 0;
}

int mdc_say(const char *text, double target_volume, const char *voice, int interruptible) {
    if (!text || !text[0]) return -1;
    if (say_ensure() != 0) return -1;
    if (target_volume <= 0 || target_volume > 1) target_volume = 0.7;

    float saved_volume = -1;
    OID category = 0;
    if (say_get_volume(&saved_volume, &category) != 0) {
        saved_volume = -1;
        category = 0;
    }
    if (category) say_set_volume((float)target_volume, category);
    say_audio_session(interruptible ? "AVAudioSessionCategoryPlayAndRecord"
                                    : "AVAudioSessionCategoryPlayback", 1);

    OID utterance_class = rt_get_class("AVSpeechUtterance");
    OID ns_text = mdc_str(text);
    OID utterance = (utterance_class && ns_text)
        ? rt_msg1(utterance_class, rt_sel("speechUtteranceWithString:"), ns_text)
        : 0;
    if (!utterance) {
        say_audio_session("AVAudioSessionCategoryPlayAndRecord", 0);
        if (category && saved_volume >= 0) say_set_volume(saved_volume, category);
        return -1;
    }
    OID voice_obj = say_pick_voice(voice);
    if (voice_obj) rt_void1(utterance, rt_sel("setVoice:"), voice_obj);

    /* Optional barge-in monitor: a metering recorder that watches the mic
     * while the synthesizer speaks. */
    OID barge = 0;
    if (interruptible) {
        barge = make_wav_recorder("/var/mobile/Downloads/.psychosis-barge.wav");
        if (barge) {
            ((void (*)(OID, OSEL, signed char))g_msg_send)(
                barge, rt_sel("setMeteringEnabled:"), 1);
            if (!((signed char (*)(OID, OSEL))g_msg_send)(barge, rt_sel("record")))
                barge = 0;
        }
    }

    pthread_mutex_lock(&g_say_lock);
    g_say_done = 0;
    pthread_mutex_unlock(&g_say_lock);
    rt_void1(g_say_synth, rt_sel("speakUtterance:"), utterance);

    long timeout = 10 + (long)strlen(text) / 5;
    if (timeout > 180) timeout = 180;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout;

    int interrupted = 0;
    double speech_ms = 0;
    for (;;) {
        pthread_mutex_lock(&g_say_lock);
        if (g_say_done) {
            pthread_mutex_unlock(&g_say_lock);
            break;
        }
        struct timespec soon;
        clock_gettime(CLOCK_REALTIME, &soon);
        soon.tv_nsec += PSYCHOSIS_METER_POLL_MS * 1000000L;
        if (soon.tv_nsec >= 1000000000L) {
            soon.tv_sec += 1;
            soon.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g_say_cond, &g_say_lock, &soon);
        int done_now = g_say_done;
        pthread_mutex_unlock(&g_say_lock);
        if (done_now) break;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            break;
        if (barge) {
            ((void (*)(OID, OSEL))g_msg_send)(barge, rt_sel("updateMeters"));
            float db = ((float (*)(OID, OSEL, unsigned long))g_msg_send)(
                barge, rt_sel("averagePowerForChannel:"), 0);
            if (db > PSYCHOSIS_SPEECH_DB) speech_ms += PSYCHOSIS_METER_POLL_MS;
            else speech_ms = 0;
            if (speech_ms >= 150) {
                ((void (*)(OID, OSEL, long))g_msg_send)(
                    g_say_synth, rt_sel("stopSpeakingAtBoundary:"), 0 /* immediate */);
                interrupted = 1;
                break;
            }
        }
    }
    int finished = g_say_done;

    if (barge) {
        ((void (*)(OID, OSEL))g_msg_send)(barge, rt_sel("stop"));
        unlink("/var/mobile/Downloads/.psychosis-barge.wav");
    }
    say_audio_session(interruptible ? "AVAudioSessionCategoryPlayAndRecord"
                                    : "AVAudioSessionCategoryPlayback", 0);
    if (category && saved_volume >= 0) say_set_volume(saved_volume, category);
    if (interrupted) return -3;
    return finished ? 0 : -2;
}

/* ------------------------------------------------------------------ */
/* record - microphone capture to a WAV file                           */
/* ------------------------------------------------------------------ */

int mdc_record(double seconds, char *path_io, size_t cap) {
    if (mdc_rt_init() != 0) return -1;
    if (seconds < 1) seconds = 5;
    if (seconds > 120) seconds = 120;
    void *av = dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_LAZY);
    if (!av) return -1;
    if (path_io && cap && !path_io[0])
        snprintf(path_io, cap, "%s", "/var/mobile/Downloads/psychosis-recording.wav");
    const char *path = (path_io && path_io[0]) ? path_io : "/var/mobile/Downloads/psychosis-recording.wav";

    OID recorder = make_wav_recorder(path);
    if (!recorder) return -2;

    char status_text[64];
    snprintf(status_text, sizeof(status_text),
             "\xF0\x9F\x8E\x99 recording %d s\xE2\x80\xA6", (int)seconds);
    mdc_status_show(status_text);

    say_audio_session("AVAudioSessionCategoryPlayAndRecord", 1);
    signed char started = ((signed char (*)(OID, OSEL, double))g_msg_send)(
        recorder, rt_sel("recordForDuration:"), seconds);
    if (!started) {
        say_audio_session("AVAudioSessionCategoryPlayAndRecord", 0);
        mdc_status_show("mic unavailable");
        mdc_status_autohide(2.0);
        return -3;
    }
    OSEL is_recording = rt_sel("isRecording");
    if (((signed char (*)(OID, OSEL, OSEL))g_msg_send)(
            recorder, rt_sel("respondsToSelector:"), is_recording)) {
        double waited = 0;
        while (((signed char (*)(OID, OSEL))g_msg_send)(recorder, is_recording)) {
            usleep(100000);
            waited += 0.1;
            if (waited > seconds + 5) break;
        }
    } else {
        usleep((useconds_t)(seconds * 1000000));
    }
    ((void (*)(OID, OSEL))g_msg_send)(recorder, rt_sel("stop"));
    say_audio_session("AVAudioSessionCategoryPlayAndRecord", 0);
    mdc_status_show("\xE2\x9C\x93 recorded");
    mdc_status_autohide(1.6);
    return 0;
}

/* One-shot speech-triggered recording (see header comment). */
int mdc_listen_turn(double timeout_s, double silence_ms, double min_speech_ms,
                    char *path_io, size_t cap, double *out_seconds) {
    if (out_seconds) *out_seconds = 0;
    if (mdc_rt_init() != 0) return -1;
    if (timeout_s <= 0 || timeout_s > 120) timeout_s = 20;
    if (silence_ms < 200 || silence_ms > 3000) silence_ms = 600;
    if (min_speech_ms < 100 || min_speech_ms > 2000) min_speech_ms = 300;
    void *av = dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_LAZY);
    if (!av) return -1;
    if (path_io && cap && !path_io[0])
        snprintf(path_io, cap, "%s", "/var/mobile/Downloads/psychosis-turn.wav");
    const char *path = (path_io && path_io[0]) ? path_io : "/var/mobile/Downloads/psychosis-turn.wav";

    OID recorder = make_wav_recorder(path);
    if (!recorder) return -2;
    ((void (*)(OID, OSEL, signed char))g_msg_send)(recorder, rt_sel("setMeteringEnabled:"), 1);
    say_audio_session("AVAudioSessionCategoryPlayAndRecord", 1);
    signed char started = ((signed char (*)(OID, OSEL))g_msg_send)(recorder, rt_sel("record"));
    if (!started) {
        say_audio_session("AVAudioSessionCategoryPlayAndRecord", 0);
        mdc_status_show("mic unavailable");
        mdc_status_autohide(2.0);
        return -3;
    }
    mdc_status_show("\xF0\x9F\x8E\x99 listening \xE2\x80\x94 speak now");

    typedef void (*VoidFn)(OID, OSEL);
    double waited = 0, speech_ms = 0, quiet_ms = 0, spoken_ms = 0;
    int state = 0; /* 0 wait for speech, 1 speaking, 2 done */
    while (state != 2) {
        usleep(PSYCHOSIS_METER_POLL_MS * 1000);
        double dt = PSYCHOSIS_METER_POLL_MS;
        waited += dt;
        ((VoidFn)g_msg_send)(recorder, rt_sel("updateMeters"));
        float db = ((float (*)(OID, OSEL, unsigned long))g_msg_send)(
            recorder, rt_sel("averagePowerForChannel:"), 0);
        int loud = db > PSYCHOSIS_SPEECH_DB;
        if (state == 0) {
            if (loud) {
                speech_ms += dt;
                if (speech_ms >= min_speech_ms) {
                    state = 1;
                    mdc_status_show("\xE2\x97\x8F recording");
                }
            } else {
                speech_ms = 0;
            }
            if (state == 0 && waited >= timeout_s * 1000.0) break; /* no speech */
        } else {
            spoken_ms += dt;
            if (loud) quiet_ms = 0;
            else quiet_ms += dt;
            if (quiet_ms >= silence_ms) state = 2;
        }
    }
    ((void (*)(OID, OSEL))g_msg_send)(recorder, rt_sel("stop"));
    say_audio_session("AVAudioSessionCategoryPlayAndRecord", 0);
    if (state != 2 || spoken_ms <= 0) {
        unlink(path);
        mdc_status_show("no speech heard");
        mdc_status_autohide(1.8);
        return -4;
    }
    mdc_status_show("transcribing\xE2\x80\xA6");
    mdc_status_autohide(6.0);
    if (out_seconds) *out_seconds = spoken_ms / 1000.0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* clipboard + open_url                                                */
/* ------------------------------------------------------------------ */

/* UIPasteboard reads/writes go through pasteboardd over XPC and can block
 * for a long time (observed: a get issued on SpringBoard's main thread
 * deadlocks and takes the whole board down with it -- watchdog kill).
 * Run them on a disposable worker thread with a 4s deadline: the main
 * thread and the command server stay responsive no matter what. On
 * deadline we detach the worker (it may unblock later) and report -2. */

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
    int rc;
    char *value;      /* get: strdup'ed UTF8 pasteboard string */
    const char *text; /* set: text to write */
} ClipboardJob;

static void clipboard_job_create(ClipboardJob *job) {
    memset(job, 0, sizeof(*job));
    pthread_mutex_init(&job->mutex, 0);
    pthread_cond_init(&job->cond, 0);
}

static void clipboard_job_finish(ClipboardJob *job) {
    pthread_mutex_lock(&job->mutex);
    job->done = 1;
    pthread_cond_signal(&job->cond);
    pthread_mutex_unlock(&job->mutex);
}

static OID clipboard_pool_push(void) {
    OID pool_class = rt_get_class("NSAutoreleasePool");
    return pool_class ? rt_msg0(rt_msg0(pool_class, rt_sel("alloc")), rt_sel("init")) : 0;
}

static void clipboard_pool_pop(OID pool) {
    if (pool) ((void (*)(OID, OSEL))g_msg_send)(pool, rt_sel("drain"));
}

static void *clipboard_get_worker(void *p) {
    ClipboardJob *job = p;
    job->rc = -1;
    if (mdc_rt_init() == 0) {
        OID pool = clipboard_pool_push();
        OID cls = rt_get_class("UIPasteboard");
        OID pb = cls ? rt_msg0(cls, rt_sel("generalPasteboard")) : 0;
        if (pb) {
            OID s = rt_msg0(pb, rt_sel("string"));
            const char *utf8 = s
                ? ((const char *(*)(OID, OSEL))g_msg_send)(s, rt_sel("UTF8String")) : "";
            job->value = strdup(utf8 ? utf8 : "");
            job->rc = job->value ? 0 : -1;
        }
        clipboard_pool_pop(pool);
    }
    clipboard_job_finish(job);
    return 0;
}

static void *clipboard_set_worker(void *p) {
    ClipboardJob *job = p;
    job->rc = -1;
    if (mdc_rt_init() == 0) {
        OID pool = clipboard_pool_push();
        OID cls = rt_get_class("UIPasteboard");
        OID pb = cls ? rt_msg0(cls, rt_sel("generalPasteboard")) : 0;
        OID s = pb ? mdc_str(job->text) : 0;
        if (s) {
            rt_void1(pb, rt_sel("setString:"), s);
            job->rc = 0;
        }
        clipboard_pool_pop(pool);
    }
    clipboard_job_finish(job);
    return 0;
}

/* Run worker to completion within 4s. Returns job rc, -1 on setup trouble,
 * or -2 when the deadline passed (worker detached; job leaked on purpose
 * because the worker may still touch it). */
static int clipboard_run(void *(*worker)(void *), ClipboardJob *job) {
    pthread_t thread;
    if (pthread_create(&thread, 0, worker, job) != 0) return -1;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 4;
    pthread_mutex_lock(&job->mutex);
    while (!job->done) {
        if (pthread_cond_timedwait(&job->cond, &job->mutex, &deadline) != 0) break;
    }
    int finished = job->done;
    pthread_mutex_unlock(&job->mutex);
    if (!finished) {
        pthread_detach(thread);
        return -2;
    }
    pthread_join(thread, 0);
    return job->rc;
}

static void clipboard_job_dispose(ClipboardJob *job) {
    pthread_mutex_destroy(&job->mutex);
    pthread_cond_destroy(&job->cond);
    free(job);
}

/* Cross-app pasteboard content cannot be read via UIPasteboard from
 * SpringBoard: the iOS 16 paste-permission flow has no UI to complete in
 * this process, so the XPC read just blocks. pasteboardd's storage in
 * /var/mobile/Library/Caches/com.apple.Pasteboard is readable as mobile, so
 * fall back to reading the newest item directly. Text-only heuristic: the
 * smallest non-Manifest payload that looks like UTF-8 text wins. */
static int clipboard_disk_looks_like_text(const unsigned char *data, size_t n) {
    if (n == 0 || n > 98304) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = data[i];
        if (c == 0) return 0;
        if (c < 0x20 && c != '\n' && c != '\t' && c != '\r') return 0;
    }
    return 1;
}

static int clipboard_disk_fallback(char *out, size_t cap) {
    const char *base = "/var/mobile/Library/Caches/com.apple.Pasteboard";
    DIR *dir = opendir(base);
    if (!dir) return -1;
    char newest[1200] = {0};
    time_t newest_time = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != 0) {
        if (ent->d_name[0] == '.') continue;
        char path[1400];
        if (snprintf(path, sizeof(path), "%s/%s", base, ent->d_name) >= (int)sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (st.st_mtime >= newest_time) {
            newest_time = st.st_mtime;
            snprintf(newest, sizeof(newest), "%s", path);
        }
    }
    closedir(dir);
    if (!newest[0]) return -1;

    dir = opendir(newest);
    if (!dir) return -1;
    char best[1600] = {0};
    off_t best_size = -1;
    while ((ent = readdir(dir)) != 0) {
        if (ent->d_name[0] == '.') continue;
        if (!strcmp(ent->d_name, "Manifest.plist")) continue;
        char path[1800];
        if (snprintf(path, sizeof(path), "%s/%s", newest, ent->d_name) >= (int)sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (st.st_size <= 0 || st.st_size > 98304) continue;
        if (best_size < 0 || st.st_size < best_size) {
            best_size = st.st_size;
            snprintf(best, sizeof(best), "%s", path);
        }
    }
    closedir(dir);
    if (!best[0]) return -1;

    FILE *file = fopen(best, "rb");
    if (!file) return -1;
    char *buffer = malloc(98304);
    if (!buffer) { fclose(file); return -1; }
    size_t got = fread(buffer, 1, 98304, file);
    int bad = ferror(file);
    fclose(file);
    int ok = !bad && clipboard_disk_looks_like_text((unsigned char *)buffer, got);
    if (ok) {
        size_t copy = got < cap - 1 ? got : cap - 1;
        memcpy(out, buffer, copy);
        out[copy] = 0;
    }
    free(buffer);
    return ok ? 0 : -1;
}

int mdc_clipboard_get(char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    out[0] = 0;
    ClipboardJob *job = calloc(1, sizeof(*job));
    if (!job) return -1;
    clipboard_job_create(job);
    int rc = clipboard_run(clipboard_get_worker, job);
    if (rc == -2) {
        /* Blocked on the paste-permission flow; read pasteboardd's store. */
        return clipboard_disk_fallback(out, cap) == 0 ? 0 : -2;
    }
    if (rc == 0 && job->value) snprintf(out, cap, "%s", job->value);
    free(job->value);
    clipboard_job_dispose(job);
    return rc;
}

int mdc_clipboard_set(const char *text) {
    if (!text) return -1;
    ClipboardJob *job = calloc(1, sizeof(*job));
    if (!job) return -1;
    clipboard_job_create(job);
    job->text = text;
    int rc = clipboard_run(clipboard_set_worker, job);
    if (rc == -2) return -2; /* worker leaked on purpose */
    clipboard_job_dispose(job);
    return rc;
}

typedef struct { const char *url; int rc; } OpenUrlCtx;

static void open_url_main(void *p) {
    OpenUrlCtx *ctx = p;
    OID app = 0;
    OID springboard = rt_get_class("SpringBoard");
    if (springboard) app = rt_msg0(springboard, rt_sel("sharedApplication"));
    if (!app) {
        OID app_class = rt_get_class("UIApplication");
        if (app_class) app = rt_msg0(app_class, rt_sel("sharedApplication"));
    }
    if (!app) return;
    OID url_class = rt_get_class("NSURL");
    OID ns_url = url_class && mdc_str(ctx->url)
        ? rt_msg1(url_class, rt_sel("URLWithString:"), mdc_str(ctx->url)) : 0;
    if (!ns_url) return;
    signed char (*responds)(OID, OSEL, OSEL) = (signed char (*)(OID, OSEL, OSEL))g_msg_send;
    OSEL modern = rt_sel("openURL:options:completionHandler:");
    if (responds(app, rt_sel("respondsToSelector:"), modern)) {
        OID options = rt_msg0(rt_get_class("NSDictionary"), rt_sel("dictionary"));
        ((void (*)(OID, OSEL, OID, OID, void *))g_msg_send)(app, modern, ns_url, options, 0);
        ctx->rc = 0;
        return;
    }
    OSEL legacy = rt_sel("openURL:");
    if (responds(app, rt_sel("respondsToSelector:"), legacy)) {
        ((signed char (*)(OID, OSEL, OID))g_msg_send)(app, legacy, ns_url);
        ctx->rc = 0;
    }
}

int mdc_open_url(const char *url) {
    if (!url || !url[0]) return -1;
    if (mdc_rt_init() != 0) return -1;
    OpenUrlCtx ctx = { url, -1 };
    if (mdc_run_on_main_thread_sync(open_url_main, &ctx) != 0) return -1;
    return ctx.rc;
}

/* ------------------------------------------------------------------ */
/* screen size (points)                                                */
/* ------------------------------------------------------------------ */

typedef struct { double w, h, scale; double pix_w, pix_h; int ok; } MdcScreenBox;

static void mdc_screen_size_main(void *p) {
    MdcScreenBox *box = p;
    OID cls = rt_get_class("UIScreen");
    OID screen = cls ? rt_msg0(cls, rt_sel("mainScreen")) : 0;
    if (!screen) return;
    R64 bounds = ((R64 (*)(OID, OSEL))g_msg_send)(screen, rt_sel("bounds"));
    double scale = ((double (*)(OID, OSEL))g_msg_send)(screen, rt_sel("scale"));
    if (bounds.w <= 0 || bounds.h <= 0 || scale <= 0) return;
    box->w = bounds.w;
    box->h = bounds.h;
    box->scale = scale;
    box->pix_w = box->w * scale;
    box->pix_h = box->h * scale;
    // nativeBounds is the physical panel in pixels, independent of interface
    // orientation -- what CARenderServerRenderDisplay expects for "LCD".
    if (rt_msg0 && ((signed char (*)(OID, OSEL, OID))g_msg_send)(
            screen, rt_sel("respondsToSelector:"), (OID)rt_sel("nativeBounds"))) {
        R64 native = ((R64 (*)(OID, OSEL))g_msg_send)(screen, rt_sel("nativeBounds"));
        if (native.w > 0 && native.h > 0) {
            box->pix_w = native.w;
            box->pix_h = native.h;
        }
    }
    box->ok = 1;
}

int mdc_screen_size(double *out_w, double *out_h) {
    if (mdc_rt_init() != 0) return -1;
    MdcScreenBox box;
    memset(&box, 0, sizeof(box));
    if (mdc_run_on_main_thread_sync(mdc_screen_size_main, &box) != 0 || !box.ok) return -1;
    if (out_w) *out_w = box.w;
    if (out_h) *out_h = box.h;
    return 0;
}

typedef struct { double w, h; int ok; } MdcRectBox;
typedef struct { double x, y; } P64;

static void mdc_fixed_size_main(void *p) {
    MdcRectBox *box = p;
    OID cls = rt_get_class("UIScreen");
    OID screen = cls ? rt_msg0(cls, rt_sel("mainScreen")) : 0;
    if (!screen) return;
    OID space = rt_msg0(screen, rt_sel("fixedCoordinateSpace"));
    if (!space) return;
    R64 bounds = ((R64 (*)(OID, OSEL))g_msg_send)(space, rt_sel("bounds"));
    if (bounds.w <= 0 || bounds.h <= 0) return;
    box->w = bounds.w;
    box->h = bounds.h;
    box->ok = 1;
}

int mdc_fixed_screen_size(double *out_w, double *out_h) {
    if (mdc_rt_init() != 0) return -1;
    MdcRectBox box;
    memset(&box, 0, sizeof(box));
    if (mdc_run_on_main_thread_sync(mdc_fixed_size_main, &box) != 0 || !box.ok) return -1;
    if (out_w) *out_w = box.w;
    if (out_h) *out_h = box.h;
    return 0;
}

typedef struct { double x, y, ox, oy; int ok; } MdcPointBox;

static void mdc_fixed_to_current_main(void *p) {
    MdcPointBox *box = p;
    OID cls = rt_get_class("UIScreen");
    OID screen = cls ? rt_msg0(cls, rt_sel("mainScreen")) : 0;
    if (!screen) return;
    OID current_space = rt_msg0(screen, rt_sel("coordinateSpace"));
    OID fixed_space = rt_msg0(screen, rt_sel("fixedCoordinateSpace"));
    if (!current_space || !fixed_space) return;
    P64 (*convert)(OID, OSEL, P64, OID) = (P64 (*)(OID, OSEL, P64, OID))g_msg_send;
    P64 point = { box->x, box->y };
    P64 out = convert(fixed_space, rt_sel("convertPoint:toCoordinateSpace:"), point, current_space);
    box->ox = out.x;
    box->oy = out.y;
    box->ok = 1;
}

int mdc_fixed_to_current(double x, double y, double *out_x, double *out_y) {
    if (mdc_rt_init() != 0) return -1;
    MdcPointBox box;
    memset(&box, 0, sizeof(box));
    box.x = x;
    box.y = y;
    if (mdc_run_on_main_thread_sync(mdc_fixed_to_current_main, &box) != 0 || !box.ok) return -1;
    if (out_x) *out_x = box.ox;
    if (out_y) *out_y = box.oy;
    return 0;
}

/* ------------------------------------------------------------------ */
/* shot - display capture. Capture strategy ported from the MIT-licensed
 * witchan/ios-mcp ScreenManager: ask UIKit for the screen surface
 * ([UIWindow createScreenIOSurface] + UICreateCGImageFromIOSurface),
 * then CARenderServerCaptureDisplay, then _UICreateScreenUIImage.
 * Images are downsampled to point size so coordinates read off the
 * screenshot map 1:1 onto tapname/wait coordinates. */
/* ------------------------------------------------------------------ */

typedef struct { CFRef image; int source; } ShotCaptureCtx;

static int g_shot_symbols;

static CFRef (*s_cg_image_from_surface)(CFRef);
static CFRef (*s_render_capture_display)(uint32_t, CFRef, CFRef);
static OID (*s_ui_create_screen_image)(void);
static size_t (*s_cg_image_width)(CFRef);
static size_t (*s_cg_image_height)(CFRef);
static CFRef (*s_cg_image_retain)(CFRef);
static void (*s_cg_image_release)(CFRef);
static CFRef (*s_cg_color_space)(void);
static void (*s_cg_color_space_release)(CFRef);
static CFRef (*s_cg_bitmap_context)(void *, size_t, size_t, size_t, size_t, CFRef, uint32_t);
static void (*s_cg_draw_image)(CFRef, R64, CFRef);
static CFRef (*s_cg_bitmap_context_image)(CFRef);
static void (*s_cg_context_release)(CFRef);
static CFRef (*s_cfstr)(CFRef, const char *, unsigned int);
static CFRef (*s_cfurl)(CFRef, const unsigned char *, long, bool);
static void (*s_cfrelease)(CFRef);
static CFRef (*s_dest_create)(CFRef, CFRef, long, CFRef);
static void (*s_dest_add)(CFRef, CFRef, CFRef);
static bool (*s_dest_finalize)(CFRef);

static int shot_load_symbols(void) {
    if (g_shot_symbols) return g_shot_symbols < 0 ? -1 : 0;
    g_shot_symbols = -1;
    void *quartz = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_LAZY);
    void *imageio = dlopen("/System/Library/Frameworks/ImageIO.framework/ImageIO", RTLD_LAZY);
    if (!imageio) return -1;
    s_cg_image_from_surface = dlsym(RTLD_DEFAULT, "UICreateCGImageFromIOSurface");
    s_ui_create_screen_image = dlsym(RTLD_DEFAULT, "_UICreateScreenUIImage");
    s_render_capture_display = quartz ? dlsym(quartz, "CARenderServerCaptureDisplay") : 0;
    s_cg_image_width = dlsym(RTLD_DEFAULT, "CGImageGetWidth");
    s_cg_image_height = dlsym(RTLD_DEFAULT, "CGImageGetHeight");
    s_cg_image_retain = dlsym(RTLD_DEFAULT, "CGImageRetain");
    s_cg_image_release = dlsym(RTLD_DEFAULT, "CGImageRelease");
    s_cg_color_space = dlsym(RTLD_DEFAULT, "CGColorSpaceCreateDeviceRGB");
    s_cg_color_space_release = dlsym(RTLD_DEFAULT, "CGColorSpaceRelease");
    s_cg_bitmap_context = dlsym(RTLD_DEFAULT, "CGBitmapContextCreate");
    s_cg_draw_image = dlsym(RTLD_DEFAULT, "CGContextDrawImage");
    s_cg_bitmap_context_image = dlsym(RTLD_DEFAULT, "CGBitmapContextCreateImage");
    s_cg_context_release = dlsym(RTLD_DEFAULT, "CGContextRelease");
    s_cfstr = dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
    s_cfurl = dlsym(RTLD_DEFAULT, "CFURLCreateFromFileSystemRepresentation");
    s_cfrelease = dlsym(RTLD_DEFAULT, "CFRelease");
    s_dest_create = dlsym(imageio, "CGImageDestinationCreateWithURL");
    s_dest_add = dlsym(imageio, "CGImageDestinationAddImage");
    s_dest_finalize = dlsym(imageio, "CGImageDestinationFinalize");
    if (!s_cg_image_width || !s_cg_image_height || !s_cg_image_release || !s_cfstr ||
        !s_cfurl || !s_cfrelease || !s_dest_create || !s_dest_add || !s_dest_finalize)
        return -1;
    if (!s_cg_image_from_surface && !s_render_capture_display && !s_ui_create_screen_image)
        return -1;
    g_shot_symbols = 1;
    return 0;
}

static void shot_capture_main(void *p) {
    ShotCaptureCtx *ctx = p;
    OID window_class = rt_get_class("UIWindow");
    signed char (*responds)(OID, OSEL, OSEL) = (signed char (*)(OID, OSEL, OSEL))g_msg_send;

    // 1) [UIWindow createScreenIOSurface] -> UICreateCGImageFromIOSurface
    if (window_class && s_cg_image_from_surface) {
        OSEL sel = rt_sel("createScreenIOSurface");
        if (responds(window_class, rt_sel("respondsToSelector:"), sel)) {
            CFRef surface = ((OID (*)(OID, OSEL))g_msg_send)(window_class, sel);
            if (surface) {
                CFRef image = s_cg_image_from_surface(surface);
                s_cfrelease(surface);
                if (image) { ctx->image = image; ctx->source = 1; }
            }
        }
    }
    // 2) CARenderServerCaptureDisplay(0, "LCD"/"Main", NULL)
    if (!ctx->image && s_render_capture_display) {
        static const char *names[] = { "LCD", "Main" };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !ctx->image; i++) {
            CFRef name = s_cfstr(0, names[i], 0x08000100u);
            if (!name) continue;
            CFRef image = s_render_capture_display(0, name, 0);
            s_cfrelease(name);
            if (image) { ctx->image = image; ctx->source = 2; }
        }
    }
    // 3) _UICreateScreenUIImage -> CGImage (retained)
    if (!ctx->image && s_ui_create_screen_image && s_cg_image_retain) {
        OID uiimage = s_ui_create_screen_image();
        if (uiimage) {
            OID cg = ((OID (*)(OID, OSEL))g_msg_send)(uiimage, rt_sel("CGImage"));
            if (cg) { ctx->image = s_cg_image_retain((CFRef)cg); ctx->source = 3; }
        }
    }
}

int mdc_shot(char *path_io, size_t cap, int *out_w, int *out_h) {
    if (mdc_rt_init() != 0) return -1;
    if (shot_load_symbols() != 0) return -1;
    if (path_io && cap && !path_io[0])
        snprintf(path_io, cap, "%s", "/var/mobile/Downloads/psychosis-shot.png");
    const char *path = (path_io && path_io[0]) ? path_io : "/var/mobile/Downloads/psychosis-shot.png";

    ShotCaptureCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (mdc_run_on_main_thread_sync(shot_capture_main, &ctx) != 0 || !ctx.image) {
        shot_log("capture-failed", ctx.source);
        return -2;
    }
    shot_log("capture", ctx.source);

    size_t w = s_cg_image_width(ctx.image);
    size_t h = s_cg_image_height(ctx.image);
    if (!w || !h) {
        s_cg_image_release(ctx.image);
        return -1;
    }

    // Downsample to screen-point size: one image pixel == one point, so
    // coordinates read off the screenshot are directly usable with tapname.
    size_t tw = w, th = h;
    double fw = 0, fh = 0;
    if (mdc_fixed_screen_size(&fw, &fh) == 0 && fw > 1 && fh > 1) {
        double long_points = fw > fh ? fw : fh;
        double long_pixels = (double)(w > h ? w : h);
        double ratio = long_pixels / long_points;
        if (ratio > 1.01) {
            tw = (size_t)(w / ratio + 0.5);
            th = (size_t)(h / ratio + 0.5);
        }
    }
    CFRef final_image = ctx.image;
    if ((tw != w || th != h) && s_cg_bitmap_context && s_cg_draw_image &&
        s_cg_bitmap_context_image && s_cg_color_space) {
        CFRef space = s_cg_color_space();
        CFRef context = space
            ? s_cg_bitmap_context(0, tw, th, 8, tw * 4, space,
                                  (2u /* premultipliedFirst */ | (2u << 12) /* 32Little */))
            : 0;
        if (space && s_cg_color_space_release) s_cg_color_space_release(space);
        if (context) {
            R64 rect = { 0, 0, (double)tw, (double)th };
            s_cg_draw_image(context, rect, ctx.image);
            CFRef scaled = s_cg_bitmap_context_image(context);
            if (s_cg_context_release) s_cg_context_release(context);
            if (scaled) {
                s_cg_image_release(ctx.image);
                final_image = scaled;
                shot_log("downsampled", (long)(tw * 100000 + th));
            }
        }
    }

    int result = -1;
    CFRef url = s_cfurl(0, (const unsigned char *)path, (long)strlen(path), false);
    CFRef png_type = s_cfstr(0, "public.png", 0x08000100u);
    CFRef dest = (url && png_type) ? s_dest_create(url, png_type, 1, 0) : 0;
    if (dest) {
        s_dest_add(dest, final_image, 0);
        if (s_dest_finalize(dest)) result = 0;
    }
    if (url) s_cfrelease(url);
    if (png_type) s_cfrelease(png_type);
    s_cg_image_release(final_image);
    shot_log("write", result);
    if (result != 0) return -1;
    if (out_w) *out_w = (int)tw;
    if (out_h) *out_h = (int)th;
    return 0;
}
