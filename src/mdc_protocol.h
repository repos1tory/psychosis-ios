#ifndef MDC_PROTOCOL_H
#define MDC_PROTOCOL_H

#define MDC_MAX_LINE 512
#define MDC_MAX_BUNDLE_ID 127
#define MDC_MAX_TEXT 480
#define MDC_MAX_PATH 200
#define MDC_MAX_AX_DEPTH 12
#define MDC_MAX_SWIPE_STEPS 120

typedef enum {
    MDC_CMD_INVALID = 0,
    MDC_CMD_PING,
    MDC_CMD_TAP,
    MDC_CMD_SWIPE,
    MDC_CMD_HOME,
    MDC_CMD_VOLUP,
    MDC_CMD_LAUNCH,
    MDC_CMD_AX,
    MDC_CMD_TYPE,
    MDC_CMD_NOTIFY,
    MDC_CMD_SHOT,
    MDC_CMD_TAPNAME,
    MDC_CMD_WAIT,
    MDC_CMD_FRONT,
    MDC_CMD_WAKE,
    MDC_CMD_UNLOCK,
    MDC_CMD_SCROLL,
    MDC_CMD_TOAST,
    MDC_CMD_SAY,
    MDC_CMD_RECORD,
    MDC_CMD_LISTEN_TURN,
    MDC_CMD_LONGPRESS,
    MDC_CMD_DOUBLETAP,
    MDC_CMD_DRAG,
    MDC_CMD_CLIPBOARD_GET,
    MDC_CMD_CLIPBOARD_SET,
    MDC_CMD_KEY,
    MDC_CMD_OPEN_URL,
    MDC_CMD_SIZE,
    MDC_CMD_SCROLLTO,
    MDC_CMD_SCROLLTOP,
    MDC_CMD_SCROLLBOTTOM,
    MDC_CMD_SCROLLOFFSET
} MdcCommandType;

typedef struct {
    MdcCommandType type;
    double x1, y1, x2, y2;
    double width, height;
    /* 1 when the caller passed an explicit W H. Without it, coordinate
     * commands interpret X Y in the device's detected current screen space
     * (the same space ax frames use). */
    int has_dimensions;
    /* SCROLL/SCROLLTOP/SCROLLBOTTOM/SCROLLTO: how long to wait for layout to
     * settle before answering, in ms. 1000 default, 0 = off (--fast). */
    int settle_ms;
    int steps;
    int depth;
    int direction; /* SCROLL: 0 up, 1 down, 2 left, 3 right */
    double amount; /* SCROLL: points along the axis (0 = default); SAY: target volume (0 = default) */
    char voice[64]; /* SAY: voice name/identifier substring */
    int interruptible;   /* SAY: stop speaking when the user talks */
    double timeout;      /* LISTEN_TURN: seconds to wait for speech */
    double silence_ms;   /* LISTEN_TURN: trailing silence that ends the turn */
    double min_speech_ms;/* LISTEN_TURN: minimum speech to trigger recording */
    double hold_ms;      /* LONGPRESS/DRAG: press duration before acting */
    char bundle_id[MDC_MAX_BUNDLE_ID + 1];
    char text[MDC_MAX_TEXT + 1];
    char path[MDC_MAX_PATH + 1];
} MdcCommand;

/* Returns 0 for a valid command and -1 for invalid input. `error` receives
 * a short machine-readable code (for example, "out-of-bounds"). */
int mdc_parse_line(const char *line, MdcCommand *out, char *error, unsigned long error_size);

#endif
