/* The bug capture (F6, by way of ticket.c; MHFU_TEST_CAPTURE=frame,... at those game frames): one writes a numbered folder under
 * ~/.local/share/mhfu-recomp/captures (MHFU_CAPTUREDIR elsewhere):
 *   state1.bin      a save state, not in the 9 slots (savestate.c state_request_capture), the same file format
 *   screenshot.png  the frame as shown, at the shown resolution, the panels on it
 *   info.txt        when, the build, the settings, where the game is, the hunter, every object
 *   inputs.txt, frame-times.txt, events.txt  the last 10 s
 *   env.txt         MHFU_ environment, for tools/capture.py to run it again
 *   frame-*.txt     held (F8) only: the draws, vertices and bones of the frame shown and either side (gpu.c dump_frame)
 *   console.txt     what the run printed, the last of it, with a window (ticket.c console_start)
 *   hits.log, stalls.log  the last of the hit overlay's and the stall recorder's logs, if they're on
 *
 * It must not change the game. Everything it touches is the host's: rings kept apart from what a state saves, reads of the
 * game's memory, files. On the game's thread it takes the state's copy (as F2 does) and the frame's read back; the files
 * are written by a thread of its own. The state is taken at the next point every thread waits, usually within the frame;
 * info.txt says which frame it has, and the rest are from the frame shown as the key was pressed.
 *
 * To load one: MHFU_STATEDIR=<the folder> MHFU_STATE_LOAD=30,1 build/mhfu, or tools/capture.py (docs/runtime.md). */
#include "capture.h"
#include "ticket.h"

const char *hit_view_log_path(void);
const char *stall_log_path(void);
void hit_view_log_flush(void);
#include "inspect.h"
#include "objects.h"
#include "perf.h"
#include "savestate.h"
#include "hle.h"
#include "git_commit.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char *rt_settings_path;

void window_note(const char *text);
uint64_t wall_time_us(void);
unsigned long present_frame_number(void);
uint32_t *render_readback_present(uint32_t addr, int w, int h);
void render_shown_size(int *w, int *h);
void present_write_png(const char *path, const uint32_t *pixels, int w, int h);
double rt_game_30ths(void);
float rt_frame_30ths(void);
extern uint32_t rt_local_player;
void inspect_write(FILE *f);

#define HISTORY_US 10000000u            /* what is written: the last 10 s */

/* -- the rings: inputs, game events (always kept; a few words a game frame) ------------------------------------------ */

typedef struct { uint64_t at; unsigned long frame; uint32_t buttons; int lx, ly; float mx, my; } InputNote;
#define INPUTS 8192
static InputNote inputs[INPUTS];
static unsigned long inputs_n;

void capture_note_input(unsigned long frame, uint32_t buttons, int lx, int ly, float mx, float my) {
    static uint32_t last_b = ~0u;
    static int last_x = -1, last_y = -1;
    if (buttons == last_b && lx == last_x && ly == last_y && mx == 0 && my == 0)
        return;
    last_b = buttons, last_x = lx, last_y = ly;
    inputs[inputs_n++ % INPUTS] = (InputNote){wall_time_us(), frame, buttons, lx, ly, mx, my};
}

typedef enum { EV_STATE, EV_MOTION, EV_APPEARED, EV_GONE, EV_WHERE } EventKind;
typedef struct { uint64_t at; unsigned long frame; uint32_t addr, vtable; int kind, a0, a1, b0, b1; } Event;
#define EVENTS 8192
static Event events[EVENTS];
static unsigned long events_n;

static void event(unsigned long frame, EventKind kind, uint32_t addr, uint32_t vtable, int a0, int a1, int b0, int b1) {
    events[events_n++ % EVENTS] = (Event){wall_time_us(), frame, addr, vtable, kind, a0, a1, b0, b1};
}

/* The hunters' and monsters' state, motion: changes noted, and who came and went (lists 2 and 3) */
#define TRACKED 64
static struct { uint32_t addr, vtable; int group, state, motion, seen; } tracked[TRACKED];
static int ntracked, where_stage = -2, where_quest;
static int test_read_done;
static unsigned long test_frames[32];
static int test_n;

static void track_events(unsigned long frame) {
    uint32_t mgr = LW(0x08A62D7Cu);
    int stage, map, quest;
    game_where(&stage, &map, &quest);
    if (stage != where_stage || quest != where_quest) {
        event(frame, EV_WHERE, 0, 0, where_stage, where_quest, stage, quest);
        where_stage = stage;
        where_quest = quest;
    }
    if (mgr < 0x08800000u || mgr >= 0x09F00000u)
        return;
    for (int i = 0; i < ntracked; i++)
        tracked[i].seen = 0;
    for (uint32_t list = 2; list <= 3; list++) {
        uint32_t a = LW(mgr + 0x80028u + 4 * list);
        for (int steps = 0; a >= 0x08800000u && a < 0x09FFF000u && !(a & 3) && steps < 512; steps++, a = LW(a + 0x1C4)) {
            uint32_t vt = LW(a);
            int group = LBU(a + 0x298), state = LBU(a + 0x299), motion = LHU(a + 0x324), i = 0;
            while (i < ntracked && (tracked[i].addr != a || tracked[i].vtable != vt))
                i++;
            if (i == ntracked) {
                if (ntracked == TRACKED)
                    continue;
                tracked[ntracked++] = (typeof(tracked[0])){a, vt, group, state, motion, 1};
                event(frame, EV_APPEARED, a, vt, group, state, motion, 0);
                continue;
            }
            tracked[i].seen = 1;
            if (tracked[i].group != group || tracked[i].state != state) {
                event(frame, EV_STATE, a, vt, tracked[i].group, tracked[i].state, group, state);
                tracked[i].group = group, tracked[i].state = state;
            }
            if (tracked[i].motion != motion) {
                event(frame, EV_MOTION, a, vt, tracked[i].motion, 0, motion, 0);
                tracked[i].motion = motion;
            }
        }
    }
    for (int i = 0; i < ntracked;) {
        if (!tracked[i].seen) {
            event(frame, EV_GONE, tracked[i].addr, tracked[i].vtable, 0, 0, 0, 0);
            tracked[i] = tracked[--ntracked];
        } else
            i++;
    }
}

/* -- the folder ------------------------------------------------------------------------------------------------------- */

static void make_path(char *dir) {
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(dir, 0755);
            *p = '/';
        }
    mkdir(dir, 0755);
}

const char *capture_dir(void) {
    static char dir[3900];
    if (dir[0])
        return dir;
    const char *env = getenv("MHFU_CAPTUREDIR");
    if (env)
        snprintf(dir, sizeof dir, "%s", env);
    else if (getenv("XDG_DATA_HOME"))
        snprintf(dir, sizeof dir, "%s/mhfu-recomp/captures", getenv("XDG_DATA_HOME"));
    else
        snprintf(dir, sizeof dir, "%s/.local/share/mhfu-recomp/captures", getenv("HOME") ? getenv("HOME") : ".");
    make_path(dir);
    return dir;
}

/* The next free number: one past the highest there is, the folder made (the number kept if another run takes it) */
static int next_folder(char *out, size_t size) {
    const char *dir = capture_dir();
    int top = 0;
    DIR *d = opendir(dir);
    if (d) {
        for (struct dirent *e; (e = readdir(d));) {
            char *end;
            long v = strtol(e->d_name, &end, 10);
            if (!*end && v > top && v < 1000000)
                top = (int)v;
        }
        closedir(d);
    }
    for (int n = top + 1; n < top + 100; n++) {
        snprintf(out, size, "%s/%04d", dir, n);
        if (mkdir(out, 0755) == 0)
            return n;
        if (errno != EEXIST)
            break;
    }
    return 0;
}

/* -- a capture in progress --------------------------------------------------------------------------------------------- */

typedef struct {
    char folder[4000];
    int number;
    char *info, *inputs, *times, *events, *env;     /* (text made on the game's thread, written off it) */
    size_t info_n, inputs_n, times_n, events_n, env_n;
    uint32_t *shot;
    int shot_w, shot_h, shot_owned;
    unsigned long state_frame;
    int state_taken, state_late;
} Capture;

static Capture *current;                /* the one being gathered (at most one at a time) */
static int want_shot;
static char last_folder[4200];
static int last_number;
static unsigned long current_since;
static _Atomic int writers;

static void write_text(const char *folder, const char *name, const char *text, size_t n) {
    char path[4200];
    snprintf(path, sizeof path, "%s/%s", folder, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "capture: cannot write %s (%s)\n", path, strerror(errno));
        return;
    }
    if (text)
        fwrite(text, 1, n, f);
    fclose(f);
}

/* The last of a log, into the capture */
static void copy_tail(const char *from, const char *folder, const char *name) {
    enum { KEEP = 4 * 1024 * 1024 };
    FILE *f = from ? fopen(from, "r") : NULL;
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f), n = size > KEEP ? KEEP : size;
    fseek(f, size - n, SEEK_SET);
    char *text = malloc(n > 0 ? (size_t)n : 1);
    if (text) {
        size_t got = fread(text, 1, (size_t)n, f);
        write_text(folder, name, text, got);
        free(text);
    }
    fclose(f);
}

static void *writer(void *arg) {
    Capture *k = arg;
    char path[4200];
    if (k->shot) {
        snprintf(path, sizeof path, "%s/screenshot.png", k->folder);
        present_write_png(path, k->shot, k->shot_w, k->shot_h);
    }
    write_text(k->folder, "info.txt", k->info, k->info_n);
    write_text(k->folder, "inputs.txt", k->inputs, k->inputs_n);
    write_text(k->folder, "frame-times.txt", k->times, k->times_n);
    write_text(k->folder, "events.txt", k->events, k->events_n);
    write_text(k->folder, "env.txt", k->env, k->env_n);
    {
        size_t n;
        char *console_text = console_copy(&n);
        if (console_text) {
            write_text(k->folder, "console.txt", console_text, n);
            free(console_text);
        }
        copy_tail(hit_view_log_path(), k->folder, "hits.log");
        copy_tail(stall_log_path(), k->folder, "stalls.log");
    }
    fprintf(stderr, "capture %d written: %s\n", k->number, k->folder);
    free(k->shot);
    free(k->info);
    free(k->inputs);
    free(k->times);
    free(k->events);
    free(k->env);
    free(k);
    atomic_fetch_sub(&writers, 1);
    return NULL;
}

static void wait_for_writers(void) {
    while (atomic_load(&writers) > 0) {
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, NULL);
    }
}

/* -- what is written ---------------------------------------------------------------------------------------------------- */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

extern char **environ;

static void write_environment(FILE *f, int machine) {
    int n = 0;
    for (char **e = environ; *e; e++)
        if (!strncmp(*e, "MHFU_", 5))
            n++;
    char **sorted = malloc(sizeof *sorted * (size_t)(n ? n : 1));
    n = 0;
    for (char **e = environ; *e; e++)
        if (!strncmp(*e, "MHFU_", 5))
            sorted[n++] = *e;
    qsort(sorted, (size_t)n, sizeof *sorted, cmp_str);
    for (int i = 0; i < n; i++)
        fprintf(f, machine ? "%s\n" : "  %s\n", sorted[i]);
    free(sorted);
}

static void write_settings_file(FILE *f) {
    FILE *s = rt_settings_path ? fopen(rt_settings_path, "r") : NULL;
    if (!s) {
        fprintf(f, "settings file: %s\n", rt_settings_path ? "(can't be read)" : "(none)");
        return;
    }
    fprintf(f, "settings file %s (its lines set, the environment winning):\n", rt_settings_path);
    char line[1024];
    while (fgets(line, sizeof line, s)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p && *p != '#' && *p != '\n')
            fprintf(f, "  %s", p);
    }
    fclose(s);
}

static void write_info(FILE *f, Capture *k, unsigned long frame) {
    char when[64];
    time_t now = time(NULL);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %z", localtime(&now));
    double fps, frame_ms, worst, tick;
    perf_summary(&fps, &frame_ms, &worst, &tick);
    fprintf(f, "capture %d\ntime %s\nbuild %s\n", k->number, when, MHFU_GIT_COMMIT);
    fprintf(f, "frame %lu\ngame_30ths %.3f\na_frame_is_30ths %.3f\n", frame, rt_game_30ths(), (double)rt_frame_30ths());
    {
        int gpu_view_back(void);
        if (gpu_view_back())
            fprintf(f, "picture: %d frames back from that (the state is of the game held, not of this picture)\n", gpu_view_back());
    }
    fprintf(f, "fps %.1f  frame_ms %.2f  worst_ms_10s %.1f  ticks_a_second %.1f\n", fps, frame_ms, worst, tick);
    int stage, map, quest;
    game_where(&stage, &map, &quest);
    fprintf(f, "stage %d  map %d  quest %d (GameState +0x6AF0E, +0x6AF10, +0x6AF12)\n", stage, map, quest);
    GameObject h;
    if (rt_local_player && object_read(rt_local_player, 2, &h)) {
        fprintf(f, "hunter (the local player, as last ticked): ");
        object_print(f, &h);
    } else
        fprintf(f, "hunter: none ticked yet\n");
    /* the screenshot's and the state's: filled in when both are done (below) */
    if (k->shot)
        fprintf(f, "screenshot screenshot.png %dx%d\n", k->shot_w, k->shot_h);
    else
        fprintf(f, "screenshot: none (no frame could be read back)\n");
    static GameObject objects[2048];
    int n = objects_read(objects, 2048);
    fprintf(f, "\nobjects (%d, from the manager's lists; class names and species as classes.toml says: inferred or guess unless confirmed):\n", n);
    for (int i = 0; i < n; i++)
        object_print(f, &objects[i]);
    fprintf(f, "\nthe click inspector's selection:\n");
    inspect_write(f);
    fprintf(f, "\nsettings in effect (the MHFU_ environment, the settings file's lines already in it):\n");
    write_environment(f, 0);
    write_settings_file(f);
}

static void write_inputs(FILE *f, uint64_t now) {
    fprintf(f, "# the last 10 s of what the game was given, as a recording's lines: <frame> <buttons hex> <stick x> <stick y> <mouse x> <mouse y>\n");
    unsigned long have = inputs_n < INPUTS ? inputs_n : INPUTS, n = 0;
    while (n < have && now - inputs[(inputs_n - 1 - n) % INPUTS].at <= HISTORY_US)
        n++;
    for (unsigned long i = n; i-- > 0;) {
        const InputNote *in = &inputs[(inputs_n - 1 - i) % INPUTS];
        fprintf(f, "%lu %x %d %d %.9g %.9g\n", in->frame, in->buttons, in->lx, in->ly, (double)in->mx, (double)in->my);
    }
}

static void write_events(FILE *f, uint64_t now) {
    fprintf(f, "# the last 10 s: state and motion changes of hunters and monsters (lists 2, 3), objects appearing and going, the stage changing\n"
               "# <seconds before the capture> <frame> <what>\n");
    unsigned long have = events_n < EVENTS ? events_n : EVENTS, n = 0;
    while (n < have && now - events[(events_n - 1 - n) % EVENTS].at <= HISTORY_US)
        n++;
    for (unsigned long i = n; i-- > 0;) {
        const Event *e = &events[(events_n - 1 - i) % EVENTS];
        fprintf(f, "-%.3f %lu ", (double)(now - e->at) / 1e6, e->frame);
        switch (e->kind) {
        case EV_STATE: fprintf(f, "%08X vt %08X state %d/%d -> %d/%d\n", e->addr, e->vtable, e->a0, e->a1, e->b0, e->b1); break;
        case EV_MOTION: fprintf(f, "%08X vt %08X motion 0x%X -> 0x%X\n", e->addr, e->vtable, e->a0, e->b0); break;
        case EV_APPEARED: fprintf(f, "%08X vt %08X appeared, state %d/%d motion 0x%X\n", e->addr, e->vtable, e->a0, e->a1, e->b0); break;
        case EV_GONE: fprintf(f, "%08X vt %08X gone\n", e->addr, e->vtable); break;
        case EV_WHERE: fprintf(f, "stage %d quest %d -> stage %d quest %d\n", e->a0, e->a1, e->b0, e->b1); break;
        }
    }
}

/* -- the steps ---------------------------------------------------------------------------------------------------------- */

void capture_request(void) {
    if (current) {
        window_note("a capture is still being taken");
        return;
    }
    if (atomic_load(&writers) >= 4) {   /* (each holds a state's copy in memory) */
        window_note("captures still being written: wait a moment");
        return;
    }
    Capture *k = calloc(1, sizeof *k);
    k->number = next_folder(k->folder, sizeof k->folder);
    if (!k->number) {
        window_note("capture: cannot make its folder");
        fprintf(stderr, "capture: cannot make a folder in %s\n", capture_dir());
        free(k);
        return;
    }
    static int hooked;
    if (!hooked) {
        hooked = 1;
        atexit(wait_for_writers);
    }
    char path[4200], note[32];
    snprintf(path, sizeof path, "%s/state1.bin", k->folder);
    atomic_fetch_add(&writers, 1);      /* (the state's writer: one more, or none if it never gets taken) */
    state_request_capture(path);
    current = k;
    snprintf(last_folder, sizeof last_folder, "%s", k->folder);
    last_number = k->number;
    want_shot = 1;
    current_since = present_frame_number();
    snprintf(note, sizeof note, "capture %d", k->number);
    window_note(note);
}

int capture_wants_shot(void) { return want_shot; }

/* (ticket.c) nothing being taken or written */
int capture_idle(void) { return !current && atomic_load(&writers) == 0; }
void capture_wait(void) { wait_for_writers(); }
const char *capture_last_folder(void) { return last_folder; }
int capture_last_number(void) { return last_number; }

int capture_state_pending(void) { return current && !current->state_taken; }

void capture_state_written(void) {
    atomic_fetch_sub(&writers, 1);
}

void capture_state_taken(unsigned long frame, int late) {
    if (!current)
        return;
    current->state_taken = 1;
    current->state_frame = frame;
    current->state_late = late;
}

/* The state and the frame both had: on to the files */
static void finish(void) {
    Capture *k = current;
    current = NULL;
    hit_view_log_flush();
    FILE *f = open_memstream(&k->inputs, &k->inputs_n);
    uint64_t now = wall_time_us();
    write_inputs(f, now);
    fclose(f);
    f = open_memstream(&k->events, &k->events_n);
    write_events(f, now);
    fclose(f);
    f = open_memstream(&k->times, &k->times_n);
    perf_write_times(f, 10);
    fclose(f);
    f = open_memstream(&k->env, &k->env_n);
    write_environment(f, 1);
    fclose(f);
    /* the state's line, added to what was gathered with the frame */
    size_t have = k->info_n;
    char line[256];
    int n = k->state_taken
        ? snprintf(line, sizeof line, "\nstate state1.bin taken at frame %lu (%d frames after the frame above, once no thread held C state)\n",
                   k->state_frame, k->state_late)
        : snprintf(line, sizeof line, "\nstate: none (no point came where every thread waits with no C state; or a state was loaded first)\n");
    k->info = realloc(k->info, have + (size_t)n + 1);
    memcpy(k->info + have, line, (size_t)n + 1);
    k->info_n = have + (size_t)n;
    pthread_t t;
    atomic_fetch_add(&writers, 1);
    if (pthread_create(&t, NULL, writer, k) == 0)
        pthread_detach(t);
    else
        writer(k);
}

void capture_take_shot(uint32_t addr, const uint32_t *soft_pixels) {
    Capture *k = current;
    if (!k) {
        want_shot = 0;
        return;
    }
    uint64_t t0 = wall_time_us();
    if (soft_pixels) {
        k->shot = malloc(480 * 272 * 4);
        memcpy(k->shot, soft_pixels, 480 * 272 * 4);
        k->shot_w = 480, k->shot_h = 272;
    } else {
        render_shown_size(&k->shot_w, &k->shot_h);
        k->shot = render_readback_present(addr, k->shot_w, k->shot_h);
    }
    want_shot = 0;                      /* (only now: the note box stays out of what was just drawn) */
    int gpu_view_kept(void);
    if (!soft_pixels && inspect_held() && gpu_view_kept() >= 0) {   /* (held: for looking closely; none, if no frames are recorded) */
        void gpu_dump_frames(const char *folder);
        gpu_dump_frames(k->folder);
    }
    FILE *f = open_memstream(&k->info, &k->info_n);
    write_info(f, k, present_frame_number());
    fclose(f);
    fprintf(stderr, "capture %d: the frame read back and the info gathered in %.1f ms on the game's thread\n", k->number,
            (double)(wall_time_us() - t0) / 1000);
    if (k->state_taken)
        finish();
}

void capture_tick(unsigned long frame) {
    if (!test_read_done) {
        test_read_done = 1;
        const char *e = getenv("MHFU_TEST_CAPTURE");
        for (; e && *e && test_n < 32;) {
            char *end;
            test_frames[test_n++] = strtoul(e, &end, 10);
            e = *end ? end + 1 : end;
        }
    }
    for (int i = 0; i < test_n; i++)
        if (test_frames[i] == frame)
            capture_request();
    ticket_test(frame);
    track_events(frame);
    if (current && !want_shot) {
        /* (the shot taken and its state too; or not to come: the state given up on) */
        if (!current->state_taken && frame >= current_since + 1200) {
            state_cancel_capture();
            atomic_fetch_sub(&writers, 1);
            fprintf(stderr, "capture %d: no state: no point to take it at in 1200 frames\n", current->number);
            finish();
        } else if (current->state_taken)
            finish();
    }
}
