/**
 * Test aid: scripted controller input for emulator runs (never in release ROMs).
 * Linked with --wrap on the joypad getters when JR_AUTOPLAY=<script> is given to make.
 *
 * A script is a list of "start-end:keys[@x,y]" steps in polled frames (joypad_poll calls),
 * e.g. "1900-1905:A;2400-2600:@0,80". Keys: A B Z S(tart) L R and u d l r (D-pad).
 * Later steps win where they overlap. Every emulated second the frame rate goes to the log.
 */
#include <libdragon.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

// Speed: rendered frames per 60 VIs (one emulated second), logged every second
static volatile int vis;
static int renders, vis_last;
static void on_vi(void) { vis++; }

surface_t *__real_display_try_get(void);
surface_t *__wrap_display_try_get(void) {
    surface_t *s = __real_display_try_get();
    if (s) renders++;
    return s;
}

void __real_joypad_poll(void);
void __wrap_joypad_poll(void) {
    __real_joypad_poll();
    if (step_count < 0) { parse(); register_VI_handler(on_vi); vis_last = vis; }
    if (vis - vis_last >= 60) {
        debugf("[fps] %d frames in %d VIs at step %d\n", renders, vis - vis_last, frame);
#ifdef HLE_TEST_LIGHT_CHECK
        { extern int hle_light_maxdiff; extern uint32_t hle_light_checked; debugf("[light] %lu vertices checked, max diff %d\n", (unsigned long)hle_light_checked, hle_light_maxdiff); }
#endif
        renders = 0; vis_last = vis;
#ifdef HLE_TEST_PERF    // HLE backend profile every 4 emulated seconds
        static uint32_t start; static int secs;
        if (++secs % 4 == 0) {
            extern uint32_t __hle_perf_ticks;
            extern void hle_perf_dump(uint32_t total);
            uint32_t now = TICKS_READ();
            if (start) {
                debugf("[hle] perf: rsp-hle %lu%% of CPU time\n", (unsigned long)(__hle_perf_ticks * 100ULL / (now - start)));
                hle_perf_dump(now - start);
            }
            __hle_perf_ticks = 0; start = now;
        }
#endif
    }
    frame++;
#ifdef JR_RACE      // test aid: start race JR_RACE (0-3) once the overworld runs, like the race bot does
    if (frame == 90) {
        extern void race_start(const char* completion_script, int lap_count);
        static char script[48];
        sprintf(script, "rom:/scenes/overworld.scene#race%d", JR_RACE);
        race_start(script, 3);
    }
#endif
    prev = held;
    held = (joypad_buttons_t){0};
    stick_x = stick_y = 0;
    for (int i = 0; i < step_count; i++) {
        if (frame < steps[i].start || frame > steps[i].end) continue;
        held.raw |= steps[i].keys.raw;
        if (steps[i].x || steps[i].y) { stick_x = steps[i].x; stick_y = steps[i].y; }
    }
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
