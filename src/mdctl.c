// mdctl - host/on-device client for MDControl.dylib (127.0.0.1:6001).
#include "mdc_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void usage(const char *name) {
    fprintf(stderr,
        "usage: %s ping | tap X Y [W H] | swipe X1 Y1 X2 Y2 [steps [W H]] |\n"
        "       %s home | volup | launch BUNDLE_ID | ax [depth] | type TEXT |\n"
        "       %s notify TEXT | shot [PATH] | front | wake | unlock [PASSCODE] |\n"
        "       %s tapname LABEL | wait [--gone] LABEL | scroll up|down|left|right [POINTS] [W H] |\n"
        "       %s toast TEXT | say [v=VOL] [voice=NAME] TEXT (say voices lists) |\n"
        "       %s record [SECONDS] [PATH] | listen [TIMEOUT_S] [SILENCE_MS] [MIN_SPEECH_MS] |\n"
        "       %s longpress X Y [MS] [W H] | doubletap X Y [W H] |\n"
        "       %s drag X1 Y1 X2 Y2 [HOLD_MS] [STEPS] [W H] |\n"
        "       %s clipboard get | clipboard set TEXT | key [cmd+][shift+]NAME | open_url URL\n"
        "Coordinates use a 768x1024 portrait screen by default.\n",
        name, name, name, name, name, name, name, name, name);
}

static int send_all(int fd, const char *data, size_t length) {
    while (length) {
        ssize_t n = send(fd, data, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n;
        length -= (size_t)n;
    }
    return 0;
}

/* Resolve a relative path token in `line` against the current directory.
 * The tweak executes inside SpringBoard, whose cwd is not the caller's, so
 * relative paths would otherwise be written somewhere useless. */
static void absolutize_path(char *line, size_t cap, const char *path) {
    if (!path || !path[0] || path[0] == '/') return;
    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) return;
    char absolute[sizeof(cwd) + MDC_MAX_PATH + 2];
    int n = snprintf(absolute, sizeof(absolute), "%s/%s", cwd, path);
    if (n <= 0 || (size_t)n >= sizeof(absolute)) return;
    char *at = strstr(line, path);
    if (!at) return;
    if (at != line && at[-1] != ' ') return; /* must start at a token boundary */
    size_t plen = strlen(path);
    size_t tail = strlen(at + plen);
    if ((size_t)(at - line) + (size_t)n + tail + 1 > cap) return;
    memmove(at + (size_t)n, at + plen, tail + 1);
    memcpy(at, absolute, (size_t)n);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    char line[MDC_MAX_LINE + 2];
    size_t used = 0;
    for (int i = 1; i < argc; i++) {
        size_t separator = i > 1 ? 1 : 0;
        size_t arg_length = strlen(argv[i]);
        if (used + separator + arg_length > MDC_MAX_LINE) {
            fprintf(stderr, "err line-too-long\n");
            return 2;
        }
        if (separator) line[used++] = ' ';
        memcpy(line + used, argv[i], arg_length);
        used += arg_length;
    }
    line[used] = 0;

    MdcCommand parsed;
    char error[64];
    if (mdc_parse_line(line, &parsed, error, sizeof(error)) != 0) {
        fprintf(stderr, "err %s\n", error);
        usage(argv[0]);
        return 2;
    }
    /* Path-taking commands run inside SpringBoard, whose cwd is not ours:
     * resolve relative paths against the shell's cwd before sending. */
    if (parsed.type == MDC_CMD_SHOT || parsed.type == MDC_CMD_RECORD)
        absolutize_path(line, sizeof(line), parsed.path);
    used = strlen(line); /* absolutize may have changed the length */
    line[used++] = '\n';
    line[used] = 0;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("mdctl: socket"); return 1; }
    struct timeval receive_timeout = {
        .tv_sec = (parsed.type == MDC_CMD_AX || parsed.type == MDC_CMD_TYPE) ? 60
                   : parsed.type == MDC_CMD_SAY ? 200
                   : parsed.type == MDC_CMD_RECORD ? 140
                   : parsed.type == MDC_CMD_LISTEN_TURN
                       ? (int)(parsed.timeout > 0 ? parsed.timeout : 20.0) + 20
                   : (parsed.type == MDC_CMD_WAIT ? 45 : 15),
        .tv_usec = 0
    };
    struct timeval send_timeout = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(6001);
    if (connect(s, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("mdctl: connect 127.0.0.1:6001 (is Psychosis loaded?)");
        close(s);
        return 1;
    }
    if (send_all(s, line, used) != 0) {
        perror("mdctl: send"); close(s); return 1;
    }

    char response[4096];
    int failed = 0;
    size_t initial = 0;
    char prefix[4] = {0};
    for (;;) {
        ssize_t n = recv(s, response, sizeof(response), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { perror("mdctl: recv"); close(s); return 1; }
        if (n == 0) break;
        if (initial < sizeof(prefix)) {
            size_t add = (size_t)n < sizeof(prefix) - initial ? (size_t)n : sizeof(prefix) - initial;
            memcpy(prefix + initial, response, add);
            initial += add;
            if (parsed.type != MDC_CMD_AX && initial == sizeof(prefix) && !memcmp(prefix, "err ", 4))
                failed = 1;
        }
        if (fwrite(response, 1, (size_t)n, stdout) != (size_t)n) {
            perror("mdctl: stdout"); close(s); return 1;
        }
        if (parsed.type != MDC_CMD_AX && parsed.type != MDC_CMD_CLIPBOARD_GET &&
            memchr(response, '\n', (size_t)n)) break;
    }
    close(s);
    return failed ? 3 : 0;
}
