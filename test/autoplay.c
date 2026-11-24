/**
 * Test aid: scripted controller input for emulator runs (never in release ROMs).
 * Linked with --wrap on the joypad getters when JR_AUTOPLAY=<script> is given to make.
 *
 * A script is a list of "start-end:keys[@x,y]" steps in polled frames (joypad_poll calls),
 * e.g. "1900-1905:A;2400-2600:@0,80". Keys: A B Z S(tart) L R and u d l r (D-pad).
 * Later steps win where they overlap. Every second the frame and time go to the log.
 */
#include <libdragon.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int start, end; joypad_buttons_t keys; int8_t x, y; } step_t;

static step_t steps[64];
static int step_count = -1;
static int frame;
static joypad_buttons_t held, prev;
static int8_t stick_x, stick_y;

static void parse(void) {
    step_count = 0;
    const char *p = JR_AUTOPLAY;
    while (*p && step_count < 64) {
        step_t *s = &steps[step_count++];
        memset(s, 0, sizeof(*s));
        s->start = strtol(p, (char**)&p, 10);
        s->end = (*p == '-') ? strtol(p + 1, (char**)&p, 10) : s->start;
        if (*p == ':') p++;
        for (; *p && *p != ';' && *p != '@'; p++) switch (*p) {
            case 'A': s->keys.a = 1; break;       case 'B': s->keys.b = 1; break;
            case 'Z': s->keys.z = 1; break;       case 'S': s->keys.start = 1; break;
            case 'L': s->keys.l = 1; break;       case 'R': s->keys.r = 1; break;
            case 'u': s->keys.d_up = 1; break;    case 'd': s->keys.d_down = 1; break;
            case 'l': s->keys.d_left = 1; break;  case 'r': s->keys.d_right = 1; break;
        }
        if (*p == '@') {
            s->x = strtol(p + 1, (char**)&p, 10);
            if (*p == ',') s->y = strtol(p + 1, (char**)&p, 10);
        }
        if (*p == ';') p++;
    }
}

void __real_joypad_poll(void);
void __wrap_joypad_poll(void) {
    __real_joypad_poll();
    if (step_count < 0) parse();
    frame++;
    prev = held;
    held = (joypad_buttons_t){0};
    stick_x = stick_y = 0;
    for (int i = 0; i < step_count; i++) {
        if (frame < steps[i].start || frame > steps[i].end) continue;
        held.raw |= steps[i].keys.raw;
        if (steps[i].x || steps[i].y) { stick_x = steps[i].x; stick_y = steps[i].y; }
    }
    if (frame % 60 == 0) debugf("[autoplay] frame %d t %lu ms\n", frame, (unsigned long)TICKS_TO_MS(get_ticks()));
}

joypad_buttons_t __wrap_joypad_get_buttons(joypad_port_t port) { return port ? (joypad_buttons_t){0} : held; }
joypad_buttons_t __wrap_joypad_get_buttons_held(joypad_port_t port) { return port ? (joypad_buttons_t){0} : held; }
joypad_buttons_t __wrap_joypad_get_buttons_pressed(joypad_port_t port) {
    return port ? (joypad_buttons_t){0} : (joypad_buttons_t){ .raw = held.raw & ~prev.raw };
}
joypad_buttons_t __wrap_joypad_get_buttons_released(joypad_port_t port) {
    return port ? (joypad_buttons_t){0} : (joypad_buttons_t){ .raw = prev.raw & ~held.raw };
}
joypad_inputs_t __wrap_joypad_get_inputs(joypad_port_t port) {
    joypad_inputs_t in = {0};
    if (port) return in;
    in.btn = held;
    in.stick_x = stick_x;
    in.stick_y = stick_y;
    return in;
}
