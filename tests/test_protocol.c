/* Host-side parser tests for the psychosis shared command parser.
 * Pure C: no iOS frameworks, so this runs with the device cc or a desktop cc.
 */
#include "../src/mdc_protocol.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

static int parse_ok(const char *line, MdcCommand *out) {
    char error[64] = {0};
    int rc = mdc_parse_line(line, out, error, sizeof(error));
    if (rc != 0) fprintf(stderr, "  parse failed for [%s]: %s\n", line, error);
    return rc == 0;
}

static int parse_error(const char *line, const char *expected) {
    MdcCommand cmd;
    char error[64] = {0};
    int rc = mdc_parse_line(line, &cmd, error, sizeof(error));
    if (rc == 0) {
        fprintf(stderr, "  expected error %s for [%s], parser accepted it\n", expected, line);
        return 0;
    }
    if (strcmp(error, expected) != 0) {
        fprintf(stderr, "  expected error %s for [%s], got %s\n", expected, line, error);
        return 0;
    }
    return 1;
}

int main(void) {
    MdcCommand cmd;

    /* No W H: the parser accepts points beyond any portrait default, because
     * the server checks them against the live auto-detected space. */
    if (parse_ok("tap 1023 767", &cmd)) {
        CHECK(cmd.type == MDC_CMD_TAP, "tap type");
        CHECK(cmd.has_dimensions == 0, "tap without W H has no fixed dimensions");
        CHECK(cmd.x1 == 1023 && cmd.y1 == 767, "tap point kept");
    }
    CHECK(parse_error("tap -1 5", "out-of-bounds"), "negative point rejected");
    CHECK(parse_error("tap 1000 20 768 1024", "out-of-bounds"), "explicit space bounds enforced");

    if (parse_ok("tap 100 200 1024 768", &cmd)) {
        CHECK(cmd.has_dimensions == 1, "explicit W H recorded");
        CHECK(cmd.width == 1024 && cmd.height == 768, "explicit W H values");
    }

    if (parse_ok("swipe 1 2 1023 767", &cmd)) {
        CHECK(cmd.type == MDC_CMD_SWIPE && cmd.steps == 20, "swipe defaults");
        CHECK(cmd.has_dimensions == 0, "swipe auto space");
    }
    if (parse_ok("swipe 1 2 3 4 12 1024 768", &cmd)) {
        CHECK(cmd.steps == 12 && cmd.has_dimensions == 1, "swipe explicit steps and space");
    }
    CHECK(parse_error("swipe 1 2 3 900 20 1024 768", "out-of-bounds"), "swipe bounds");

    if (parse_ok("longpress 400 500", &cmd)) {
        CHECK(cmd.type == MDC_CMD_LONGPRESS && cmd.has_dimensions == 0,
              "longpress default space");
    }
    if (parse_ok("longpress 400 500 900", &cmd))
        CHECK(cmd.hold_ms == 900 && cmd.has_dimensions == 0, "longpress hold without space");
    if (parse_ok("longpress 400 500 900 1024 768", &cmd))
        CHECK(cmd.hold_ms == 900 && cmd.has_dimensions == 1, "longpress hold with space");
    if (parse_ok("doubletap 400 500", &cmd))
        CHECK(cmd.type == MDC_CMD_DOUBLETAP && cmd.has_dimensions == 0, "doubletap auto space");
    if (parse_ok("doubletap 400 500 1024 768", &cmd))
        CHECK(cmd.has_dimensions == 1, "doubletap explicit space");

    if (parse_ok("drag 10 20 900 700", &cmd))
        CHECK(cmd.type == MDC_CMD_DRAG && cmd.has_dimensions == 0, "drag auto space");
    if (parse_ok("drag 10 20 900 700 500 18 1024 768", &cmd))
        CHECK(cmd.hold_ms == 500 && cmd.steps == 18 && cmd.has_dimensions == 1,
              "drag explicit hold/steps/space");

    if (parse_ok("size", &cmd)) CHECK(cmd.type == MDC_CMD_SIZE, "size command");

    if (parse_ok("scroll down", &cmd)) {
        CHECK(cmd.type == MDC_CMD_SCROLL && cmd.direction == 1, "scroll down");
        CHECK(cmd.amount == 0 && cmd.settle_ms == 1000, "scroll defaults settle on");
        CHECK(cmd.has_dimensions == 0, "scroll auto space");
    }
    if (parse_ok("scroll down 600", &cmd))
        CHECK(cmd.amount == 600, "scroll points");
    if (parse_ok("scroll up 600 768 1024", &cmd))
        CHECK(cmd.has_dimensions == 1 && cmd.width == 768, "scroll explicit space");
    if (parse_ok("scroll left 600 --fast", &cmd))
        CHECK(cmd.direction == 2 && cmd.settle_ms == 0, "scroll --fast disables settle");
    if (parse_ok("scroll right 600 --settle=250", &cmd))
        CHECK(cmd.direction == 3 && cmd.settle_ms == 250, "scroll --settle=MS");
    if (parse_ok("scroll down 600 1024 768 --settle", &cmd))
        CHECK(cmd.settle_ms == 1000 && cmd.has_dimensions == 1, "scroll --settle default");
    CHECK(parse_error("scroll sideways", "invalid-direction"), "scroll direction");
    CHECK(parse_error("scroll down 0", "invalid-amount"), "scroll zero amount");
    CHECK(parse_error("scroll down -5", "invalid-amount"), "scroll negative amount");
    CHECK(parse_error("scroll down abc", "invalid-arguments"), "scroll junk argument");
    CHECK(parse_error("scroll down 600 --nope", "invalid-arguments"), "scroll unknown flag");

    if (parse_ok("scrolltop", &cmd))
        CHECK(cmd.type == MDC_CMD_SCROLLTOP && cmd.settle_ms == 1000, "scrolltop");
    if (parse_ok("scrollbottom --fast", &cmd))
        CHECK(cmd.type == MDC_CMD_SCROLLBOTTOM && cmd.settle_ms == 0, "scrollbottom --fast");
    if (parse_ok("scrolltop --settle=400", &cmd))
        CHECK(cmd.settle_ms == 400, "scrolltop settle override");

    if (parse_ok("scrollto See also", &cmd)) {
        CHECK(cmd.type == MDC_CMD_SCROLLTO, "scrollto type");
        CHECK(strcmp(cmd.text, "See also") == 0, "scrollto label with spaces");
        CHECK(cmd.settle_ms == 1000, "scrollto settles");
    }
    if (parse_ok("scrollto --up Back to top", &cmd))
        CHECK(strcmp(cmd.text, "--up Back to top") == 0, "scrollto direction prefix kept");
    CHECK(parse_error("scrollto", "invalid-arguments"), "scrollto needs a label");

    if (parse_ok("scrolloffset", &cmd))
        CHECK(cmd.type == MDC_CMD_SCROLLOFFSET && cmd.text[0] == 0,
              "scrolloffset without label is the anchor query");
    if (parse_ok("scrolloffset Poodle", &cmd))
        CHECK(strcmp(cmd.text, "Poodle") == 0, "scrolloffset label");

    if (parse_ok("clipboard set hello world", &cmd))
        CHECK(strcmp(cmd.text, "hello world") == 0, "clipboard text preserved");
    if (parse_ok("key cmd+shift+a", &cmd)) CHECK(cmd.type == MDC_CMD_KEY, "key command");

    CHECK(parse_error("frobnicate", "unknown-command"), "unknown command");

    char overlong[MDC_MAX_LINE + 16];
    memset(overlong, 'x', sizeof(overlong) - 1);
    overlong[sizeof(overlong) - 1] = 0;
    CHECK(parse_error(overlong, "line-too-long"), "line limit");

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
