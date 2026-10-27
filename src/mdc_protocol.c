#include "mdc_protocol.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MDC_DEFAULT_WIDTH 768.0
#define MDC_DEFAULT_HEIGHT 1024.0
#define MDC_MAX_DIMENSION 32768.0
#define MDC_MAX_TOKENS 9

static int fail(char *error, unsigned long size, const char *code) {
    if (error && size) snprintf(error, (size_t)size, "%s", code);
    return -1;
}

static int parse_number(const char *s, double *out) {
    char *end = 0;
    errno = 0;
    double value = strtod(s, &end);
    if (s == end || !end || *end || errno == ERANGE || !isfinite(value)) return -1;
    *out = value;
    return 0;
}

static int parse_int(const char *s, int *out) {
    char *end = 0;
    errno = 0;
    long value = strtol(s, &end, 10);
    if (s == end || !end || *end || errno == ERANGE || value < 1 || value > 2147483647L) return -1;
    *out = (int)value;
    return 0;
}

static int parse_point(const char *sx, const char *sy, double width, double height,
                       double *x, double *y) {
    if (parse_number(sx, x) || parse_number(sy, y)) return -1;
    if (*x < 0 || *y < 0 || *x >= width || *y >= height) return -2;
    return 0;
}

/* Point without a known target space: the server detects the live screen and
 * rejects points outside it, so the parser only rejects negatives and absurd
 * magnitudes here. */
static int parse_point_auto(const char *sx, const char *sy, double *x, double *y) {
    if (parse_number(sx, x) || parse_number(sy, y)) return -1;
    if (*x < 0 || *y < 0 || *x > MDC_MAX_DIMENSION || *y > MDC_MAX_DIMENSION) return -2;
    return 0;
}

static int parse_dimensions(const char *sw, const char *sh, double *width, double *height) {
    if (parse_number(sw, width) || parse_number(sh, height)) return -1;
    if (*width <= 0 || *height <= 0 || *width > MDC_MAX_DIMENSION || *height > MDC_MAX_DIMENSION)
        return -1;
    return 0;
}

/* --fast (0) or --settle[=MS] (default 1000 ms when MS is omitted). */
static int parse_settle_flag(const char *token, int *settle_ms) {
    if (!strcmp(token, "--fast")) { *settle_ms = 0; return 1; }
    if (!strcmp(token, "--settle")) { *settle_ms = 1000; return 1; }
    if (!strncmp(token, "--settle=", 9)) {
        double ms = 0;
        if (parse_number(token + 9, &ms) || ms < 0 || ms > 10000) return 0;
        *settle_ms = ms == 0 ? 0 : (int)(ms + 0.5);
        return 1;
    }
    return 0;
}

static int valid_bundle_id(const char *s, size_t *length) {
    size_t n = strlen(s);
    if (!n || n > MDC_MAX_BUNDLE_ID || s[0] == '.' || s[n - 1] == '.') return 0;
    int has_dot = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '.') {
            if (i && s[i - 1] == '.') return 0;
            has_dot = 1;
        } else if (!(isalnum(c) || c == '-' || c == '_')) {
            return 0;
        }
    }
    *length = n;
    return has_dot;
}

int mdc_parse_line(const char *line, MdcCommand *out, char *error, unsigned long error_size) {
    if (!line || !out) return fail(error, error_size, "invalid-arguments");
    size_t length = 0;
    while (length <= MDC_MAX_LINE && line[length]) length++;
    if (length > MDC_MAX_LINE) return fail(error, error_size, "line-too-long");

    /* `type`, `notify`, `tapname` and `wait` consume the complete remainder so
     * natural text and labels keep their spaces. */
    const char *first = line;
    while (*first && isspace((unsigned char)*first)) first++;
    {
        static const struct { const char *keyword; MdcCommandType type; } rest_commands[] = {
            {"type", MDC_CMD_TYPE},
            {"notify", MDC_CMD_NOTIFY},
            {"tapname", MDC_CMD_TAPNAME},
            {"wait", MDC_CMD_WAIT},
            {"toast", MDC_CMD_TOAST},
            {"say", MDC_CMD_SAY},
            {"scrollto", MDC_CMD_SCROLLTO},
            {"scrolloffset", MDC_CMD_SCROLLOFFSET},
        };
        for (size_t r = 0; r < sizeof(rest_commands) / sizeof(rest_commands[0]); r++) {
            size_t klen = strlen(rest_commands[r].keyword);
            if (strncmp(first, rest_commands[r].keyword, klen) != 0) continue;
            if (first[klen] && !isspace((unsigned char)first[klen])) continue;
            const char *body = first + klen;
            while (*body && isspace((unsigned char)*body)) body++;
            /* `say` may open with v=VOLUME (0..1) and/or voice=NAME tokens. */
            double say_volume = 0;
            char say_voice[64] = {0};
            int say_interruptible = 0;
            if (rest_commands[r].type == MDC_CMD_SAY) {
                for (int pass = 0; pass < 3; pass++) {
                    if (!strncmp(body, "v=", 2)) {
                        char *end = 0;
                        double v = strtod(body + 2, &end);
                        if (end && end != body + 2 && v >= 0 && v <= 1 &&
                            *end && isspace((unsigned char)*end)) {
                            say_volume = v;
                            body = end;
                            while (*body && isspace((unsigned char)*body)) body++;
                        }
                    } else if (!strncmp(body, "voice=", 6)) {
                        const char *val = body + 6;
                        const char *end = val;
                        while (*end && !isspace((unsigned char)*end)) end++;
                        size_t n = (size_t)(end - val);
                        if (n > 0 && n < sizeof(say_voice)) {
                            memcpy(say_voice, val, n);
                            say_voice[n] = 0;
                            body = end;
                            while (*body && isspace((unsigned char)*body)) body++;
                        }
                    } else if (!strncmp(body, "interruptible", 13) &&
                               (!body[13] || isspace((unsigned char)body[13]))) {
                        say_interruptible = 1;
                        body += 13;
                        while (*body && isspace((unsigned char)*body)) body++;
                    } else {
                        break;
                    }
                }
            }
            size_t text_length = strlen(body);
            while (text_length && isspace((unsigned char)body[text_length - 1])) text_length--;
            if (!text_length && rest_commands[r].type != MDC_CMD_SCROLLOFFSET)
                return fail(error, error_size, "invalid-arguments");
            if (text_length > MDC_MAX_TEXT) return fail(error, error_size, "text-too-long");
            for (size_t i = 0; i < text_length; i++) {
                if ((unsigned char)body[i] < 0x20 || body[i] == 0x7f)
                    return fail(error, error_size, "invalid-text");
            }
            MdcCommand result;
            memset(&result, 0, sizeof(result));
            memcpy(result.text, body, text_length);
            result.text[text_length] = 0;
            result.type = rest_commands[r].type;
            if (result.type == MDC_CMD_SCROLLTO) result.settle_ms = 1000;
            result.amount = say_volume;
            memcpy(result.voice, say_voice, sizeof(result.voice));
            result.interruptible = say_interruptible;
            *out = result;
            if (error && error_size) error[0] = 0;
            return 0;
        }
    }

    char copy[MDC_MAX_LINE + 1];
    memcpy(copy, line, length + 1);
    char *tokens[MDC_MAX_TOKENS];
    size_t count = 0;
    char *p = copy;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (count == MDC_MAX_TOKENS) return fail(error, error_size, "invalid-arguments");
        tokens[count++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = 0;
    }
    if (!count) return fail(error, error_size, "invalid-arguments");

    MdcCommand result;
    memset(&result, 0, sizeof(result));
    result.width = MDC_DEFAULT_WIDTH;
    result.height = MDC_DEFAULT_HEIGHT;
    result.steps = 20;
    result.depth = MDC_MAX_AX_DEPTH;

    if (!strcmp(tokens[0], "ping")) {
        if (count != 1) return fail(error, error_size, "invalid-arguments");
        result.type = MDC_CMD_PING;
    } else if (!strcmp(tokens[0], "tap")) {
        if (count != 3 && count != 5) return fail(error, error_size, "invalid-arguments");
        int point = -1;
        if (count == 5) {
            if (parse_dimensions(tokens[3], tokens[4], &result.width, &result.height))
                return fail(error, error_size, "invalid-number");
            result.has_dimensions = 1;
            point = parse_point(tokens[1], tokens[2], result.width, result.height,
                                &result.x1, &result.y1);
        } else {
            point = parse_point_auto(tokens[1], tokens[2], &result.x1, &result.y1);
        }
        if (point == -1) return fail(error, error_size, "invalid-number");
        if (point == -2) return fail(error, error_size, "out-of-bounds");
        result.type = MDC_CMD_TAP;
    } else if (!strcmp(tokens[0], "swipe")) {
        if (count != 5 && count != 6 && count != 8) return fail(error, error_size, "invalid-arguments");
        if (count == 6 || count == 8) {
            if (parse_int(tokens[5], &result.steps) || result.steps > MDC_MAX_SWIPE_STEPS)
                return fail(error, error_size, "invalid-steps");
        }
        if (count == 8) {
            if (parse_dimensions(tokens[6], tokens[7], &result.width, &result.height))
                return fail(error, error_size, "invalid-number");
            result.has_dimensions = 1;
        }
        int point = -1;
        if (result.has_dimensions) {
            point = parse_point(tokens[1], tokens[2], result.width, result.height,
                                &result.x1, &result.y1);
        } else {
            point = parse_point_auto(tokens[1], tokens[2], &result.x1, &result.y1);
        }
        if (point == -1) return fail(error, error_size, "invalid-number");
        if (point == -2) return fail(error, error_size, "out-of-bounds");
        if (result.has_dimensions) {
            point = parse_point(tokens[3], tokens[4], result.width, result.height,
                                &result.x2, &result.y2);
        } else {
            point = parse_point_auto(tokens[3], tokens[4], &result.x2, &result.y2);
        }
        if (point == -1) return fail(error, error_size, "invalid-number");
        if (point == -2) return fail(error, error_size, "out-of-bounds");
        result.type = MDC_CMD_SWIPE;
    } else if (!strcmp(tokens[0], "home")) {
        if (count != 1) return fail(error, error_size, "invalid-arguments");
        result.type = MDC_CMD_HOME;
    } else if (!strcmp(tokens[0], "volup")) {
        if (count != 1) return fail(error, error_size, "invalid-arguments");
        result.type = MDC_CMD_VOLUP;
    } else if (!strcmp(tokens[0], "launch")) {
        size_t n = 0;
        if (count != 2) return fail(error, error_size, "invalid-arguments");
        if (!valid_bundle_id(tokens[1], &n)) return fail(error, error_size, "invalid-bundle-id");
        memcpy(result.bundle_id, tokens[1], n + 1);
        result.type = MDC_CMD_LAUNCH;
    } else if (!strcmp(tokens[0], "ax")) {
        if (count > 2) return fail(error, error_size, "invalid-arguments");
        if (count == 2 && (parse_int(tokens[1], &result.depth) || result.depth > MDC_MAX_AX_DEPTH))
            return fail(error, error_size, "invalid-depth");
        result.type = MDC_CMD_AX;
    } else if (!strcmp(tokens[0], "shot")) {
        if (count > 2) return fail(error, error_size, "invalid-arguments");
        if (count == 2) {
            size_t n = strlen(tokens[1]);
            if (n > MDC_MAX_PATH) return fail(error, error_size, "path-too-long");
            for (size_t i = 0; i < n; i++) {
                unsigned char c = (unsigned char)tokens[1][i];
                if (c < 0x20 || c == 0x7f) return fail(error, error_size, "invalid-path");
            }
            memcpy(result.path, tokens[1], n + 1);
        }
        result.type = MDC_CMD_SHOT;
    } else if (!strcmp(tokens[0], "front")) {
        if (count != 1) return fail(error, error_size, "invalid-arguments");
        result.type = MDC_CMD_FRONT;
    } else if (!strcmp(tokens[0], "record")) {
        /* record [SECONDS] [PATH] */
        if (count > 3) return fail(error, error_size, "invalid-arguments");
        for (size_t t = 1; t < count; t++) {
            double value = 0;
            if (t == 1 && parse_number(tokens[t], &value) == 0 && value >= 1 && value <= 120) {
                result.amount = value;
                continue;
            }
            size_t n = strlen(tokens[t]);
            if (n > MDC_MAX_PATH) return fail(error, error_size, "path-too-long");
            for (size_t i = 0; i < n; i++) {
                unsigned char c = (unsigned char)tokens[t][i];
                if (c < 0x20 || c == 0x7f) return fail(error, error_size, "invalid-path");
            }
            memcpy(result.path, tokens[t], n + 1);
        }
        result.type = MDC_CMD_RECORD;
    } else if (!strcmp(tokens[0], "listen")) {
        /* listen [TIMEOUT_S] [SILENCE_MS] [MIN_SPEECH_MS]
         * ("listen turn ..." accepted as an alias) */
        int arg = 1;
        if (count >= 2 && !strcmp(tokens[1], "turn")) arg = 2;
        int arg_count = count - arg;
        if (arg_count > 3) return fail(error, error_size, "invalid-arguments");
        if (arg_count >= 1) {
            double v = 0;
            if (parse_number(tokens[arg], &v) || v <= 0 || v > 120)
                return fail(error, error_size, "invalid-number");
            result.timeout = v;
        }
        if (arg_count >= 2) {
            double v = 0;
            if (parse_number(tokens[arg + 1], &v) || v < 200 || v > 3000)
                return fail(error, error_size, "invalid-number");
            result.silence_ms = v;
        }
        if (arg_count >= 3) {
            double v = 0;
            if (parse_number(tokens[arg + 2], &v) || v < 100 || v > 2000)
                return fail(error, error_size, "invalid-number");
            result.min_speech_ms = v;
        }
        result.type = MDC_CMD_LISTEN_TURN;
    } else if (!strcmp(tokens[0], "longpress") || !strcmp(tokens[0], "doubletap")) {
        /* longpress X Y [MS] [W H]  |  doubletap X Y [W H] */
        int is_longpress = tokens[0][0] == 'l';
        int valid = is_longpress ? (count == 3 || count == 4 || count == 5 || count == 6)
                                 : (count == 3 || count == 5);
        if (!valid) return fail(error, error_size, "invalid-arguments");
        result.hold_ms = 500;
        int dims_at = -1;
        if (is_longpress && (count == 4 || count == 6)) {
            double ms = 0;
            if (parse_number(tokens[3], &ms) || ms < 100 || ms > 10000)
                return fail(error, error_size, "invalid-number");
            result.hold_ms = ms;
            dims_at = count == 6 ? 4 : -1;
        } else if (!is_longpress && count == 5) {
            dims_at = 3;
        } else if (is_longpress && count == 5) {
            dims_at = 3;
        }
        if (dims_at > 0) {
            if (parse_dimensions(tokens[dims_at], tokens[dims_at + 1],
                                 &result.width, &result.height))
                return fail(error, error_size, "invalid-number");
            result.has_dimensions = 1;
        }
        int point = -1;
        if (result.has_dimensions) {
            point = parse_point(tokens[1], tokens[2], result.width, result.height,
                                &result.x1, &result.y1);
        } else {
            point = parse_point_auto(tokens[1], tokens[2], &result.x1, &result.y1);
        }
        if (point == -1) return fail(error, error_size, "invalid-number");
        if (point == -2) return fail(error, error_size, "out-of-bounds");
        result.type = is_longpress ? MDC_CMD_LONGPRESS : MDC_CMD_DOUBLETAP;
    } else if (!strcmp(tokens[0], "drag")) {
        /* drag X1 Y1 X2 Y2 [HOLD_MS] [STEPS] [W H] */
        if (count < 5 || count > 9) return fail(error, error_size, "invalid-arguments");
        result.steps = 18;
        result.hold_ms = 600;
        if (count >= 6) {
            double ms = 0;
            if (parse_number(tokens[5], &ms) || ms < 100 || ms > 3000)
                return fail(error, error_size, "invalid-number");
            result.hold_ms = ms;
        }
        if (count >= 7) {
            if (parse_int(tokens[6], &result.steps) || result.steps > MDC_MAX_SWIPE_STEPS)
                return fail(error, error_size, "invalid-steps");
        }
        if (count == 9) {
            if (parse_dimensions(tokens[7], tokens[8], &result.width, &result.height))
                return fail(error, error_size, "invalid-number");
            result.has_dimensions = 1;
        }
        int point = -1;
        if (result.has_dimensions) {
            point = parse_point(tokens[1], tokens[2], result.width, result.height,
                                &result.x1, &result.y1);
        } else {
            point = parse_point_auto(tokens[1], tokens[2], &result.x1, &result.y1);
        }
        if (point == -1) return fail(error, error_size, "invalid-number");
        if (point == -2) return fail(error, error_size, "out-of-bounds");
        if (result.has_dimensions) {
            point = parse_point(tokens[3], tokens[4], result.width, result.height,
                                &result.x2, &result.y2);
        } else {
            point = parse_point_auto(tokens[3], tokens[4], &result.x2, &result.y2);
        }
        if (point == -1) return fail(error, error_size, "invalid-number");
        if (point == -2) return fail(error, error_size, "out-of-bounds");
        result.type = MDC_CMD_DRAG;
    } else if (!strcmp(tokens[0], "clipboard")) {
        if (count == 2 && !strcmp(tokens[1], "get")) {
            result.type = MDC_CMD_CLIPBOARD_GET;
        } else if (count >= 2 && !strcmp(tokens[1], "set")) {
            /* everything after "clipboard set" is the text */
            const char *body = line;
            while (*body && isspace((unsigned char)*body)) body++;
            body += strlen("clipboard");
            while (*body && isspace((unsigned char)*body)) body++;
            body += strlen("set");
            while (*body && isspace((unsigned char)*body)) body++;
            size_t text_length = strlen(body);
            while (text_length && isspace((unsigned char)body[text_length - 1])) text_length--;
            if (text_length > MDC_MAX_TEXT) return fail(error, error_size, "text-too-long");
            for (size_t i = 0; i < text_length; i++) {
                if ((unsigned char)body[i] < 0x20 && body[i] != '\t')
                    return fail(error, error_size, "invalid-text");
            }
            memcpy(result.text, body, text_length);
            result.text[text_length] = 0;
            result.type = MDC_CMD_CLIPBOARD_SET;
        } else {
            return fail(error, error_size, "invalid-arguments");
        }
    } else if (!strcmp(tokens[0], "key")) {
        /* key [mod+[mod+]]NAME   (cmd|shift|alt|ctrl, or a single character) */
        if (count != 2) return fail(error, error_size, "invalid-arguments");
        const char *spec = tokens[1];
        size_t n = strlen(spec);
        if (!n || n > 63) return fail(error, error_size, "invalid-key");
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)spec[i];
            if (c < 0x20 || c > 0x7e) return fail(error, error_size, "invalid-key");
        }
        const char *rest = spec;
        for (;;) {
            const char *plus = strchr(rest, '+');
            if (!plus) break;
            size_t seg = (size_t)(plus - rest);
            if (!((seg == 3 && !strncmp(rest, "cmd", 3)) ||
                  (seg == 5 && !strncmp(rest, "shift", 5)) ||
                  (seg == 3 && !strncmp(rest, "alt", 3)) ||
                  (seg == 4 && !strncmp(rest, "ctrl", 4))))
                return fail(error, error_size, "invalid-key");
            rest = plus + 1;
        }
        if (!rest[0] || strlen(rest) > 32) return fail(error, error_size, "invalid-key");
        memcpy(result.text, spec, n + 1);
        result.type = MDC_CMD_KEY;
    } else if (!strcmp(tokens[0], "open_url")) {
        if (count != 2) return fail(error, error_size, "invalid-arguments");
        size_t n = strlen(tokens[1]);
        if (!n || n > MDC_MAX_TEXT) return fail(error, error_size, "invalid-url");
        int has_colon = 0;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)tokens[1][i];
            if (c == ':') has_colon = 1;
            if (c < 0x20 || c == 0x7f) return fail(error, error_size, "invalid-url");
        }
        if (!has_colon || !isalpha((unsigned char)tokens[1][0]))
            return fail(error, error_size, "invalid-url");
        memcpy(result.text, tokens[1], n + 1);
        result.type = MDC_CMD_OPEN_URL;
    } else if (!strcmp(tokens[0], "wake")) {
        if (count != 1) return fail(error, error_size, "invalid-arguments");
        result.type = MDC_CMD_WAKE;
    } else if (!strcmp(tokens[0], "scroll")) {
        /* scroll up|down|left|right [POINTS] [W H] [--settle[=MS]|--fast] */
        if (count < 2 || count > 6) return fail(error, error_size, "invalid-arguments");
        if (!strcmp(tokens[1], "up")) result.direction = 0;
        else if (!strcmp(tokens[1], "down")) result.direction = 1;
        else if (!strcmp(tokens[1], "left")) result.direction = 2;
        else if (!strcmp(tokens[1], "right")) result.direction = 3;
        else return fail(error, error_size, "invalid-direction");
        result.settle_ms = 1000;
        size_t index = 2;
        if (index < count) {
            double amount = 0;
            if (parse_number(tokens[index], &amount) == 0) {
                if (amount <= 0 || amount > MDC_MAX_DIMENSION)
                    return fail(error, error_size, "invalid-amount");
                result.amount = amount;
                index++;
            }
        }
        if (index + 1 < count) {
            double probe = 0;
            if (parse_number(tokens[index], &probe) == 0) {
                if (parse_dimensions(tokens[index], tokens[index + 1],
                                     &result.width, &result.height))
                    return fail(error, error_size, "invalid-number");
                result.has_dimensions = 1;
                index += 2;
            }
        }
        while (index < count) {
            if (!parse_settle_flag(tokens[index], &result.settle_ms))
                return fail(error, error_size, "invalid-arguments");
            index++;
        }
        result.type = MDC_CMD_SCROLL;
    } else if (!strcmp(tokens[0], "scrolltop") || !strcmp(tokens[0], "scrollbottom")) {
        /* scrolltop|scrollbottom [--settle[=MS]|--fast] */
        if (count > 2) return fail(error, error_size, "invalid-arguments");
        result.settle_ms = 1000;
        if (count == 2 && !parse_settle_flag(tokens[1], &result.settle_ms))
            return fail(error, error_size, "invalid-arguments");
        result.type = !strcmp(tokens[0], "scrolltop") ? MDC_CMD_SCROLLTOP : MDC_CMD_SCROLLBOTTOM;
    } else if (!strcmp(tokens[0], "size")) {
        if (count != 1) return fail(error, error_size, "invalid-arguments");
        result.type = MDC_CMD_SIZE;
    } else if (!strcmp(tokens[0], "unlock")) {
        if (count > 2) return fail(error, error_size, "invalid-arguments");
        if (count == 2) {
            size_t n = strlen(tokens[1]);
            if (n < 1 || n > 12) return fail(error, error_size, "invalid-passcode");
            for (size_t i = 0; i < n; i++) {
                if (!isdigit((unsigned char)tokens[1][i]))
                    return fail(error, error_size, "invalid-passcode");
            }
            memcpy(result.text, tokens[1], n + 1);
        }
        result.type = MDC_CMD_UNLOCK;
    } else {
        return fail(error, error_size, "unknown-command");
    }

    *out = result;
    if (error && error_size) error[0] = 0;
    return 0;
}
