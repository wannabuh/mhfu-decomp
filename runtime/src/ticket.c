/* The ticket key (F6), and tickets as files (docs/tickets.md: tools/tickets.py reads and changes the same).
 *
 * F6 holds the game, takes a capture of the frame shown and opens a note box. Enter files it: a new ticket, or with a note
 * starting "#12", more on that one; the capture moves in, and leaves a file by its number in the captures' folder (the
 * next number counts from it, tools/capture.py follows it). Esc, or Enter with no note, keeps the capture alone. Held
 * before the key, it stays held after; held by the key, it goes on, once the Enter or Esc is let go (each is a game
 * action: the game mustn't get it). The capture's own picture is taken before the box is shown.
 *
 * MHFU_TEST_TICKET="frame,passes,note": the key at that game frame, the note given after so many passes of the hold (ESC
 * for Esc). Files go to $MHFU_TICKETDIR, else tickets beside the captures. It changes nothing of the game's but the hold. */
#include "ticket.h"
#include "capture.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int inspect_held(void);
void inspect_hold(int on);
void window_ticket_mode(int on);
void window_note(const char *text);
int capture_idle(void);
const char *capture_last_folder(void);
int capture_last_number(void);

#define NOTE_MAX 1024
static int open_box, was_held, hide_for_shot, ending, ending_key;
static char text[NOTE_MAX];
static int waiting;                     /* a note, for when its capture is written */
static char note[NOTE_MAX];
static int test_passes = -1;

static void mkdirs(const char *path) {
    char p[4096];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            mkdir(p, 0755);
            *c = '/';
        }
    mkdir(p, 0755);
}

const char *ticket_dir(void) {
    static char dir[4000];
    if (dir[0])
        return dir;
    if (getenv("MHFU_TICKETDIR")) {
        snprintf(dir, sizeof dir, "%s", getenv("MHFU_TICKETDIR"));
    } else {
        snprintf(dir, sizeof dir, "%s", capture_dir());
        char *slash = strrchr(dir, '/');
        if (slash)
            strcpy(slash + 1, "tickets");
    }
    mkdirs(dir);
    return dir;
}

static void today(char *out, size_t n) {
    time_t t = time(NULL);
    strftime(out, n, "%Y-%m-%d", localtime(&t));
}

/* The folder of ticket n, or 0; and the highest number there is */
static int find_ticket(int n, char *out, size_t size, int *top) {
    int found = 0;
    *top = 0;
    DIR *d = opendir(ticket_dir());
    if (!d)
        return 0;
    for (struct dirent *e; (e = readdir(d));) {
        int v = atoi(e->d_name);
        if (v <= 0 || !strchr(e->d_name, '-'))
            continue;
        if (v > *top)
            *top = v;
        if (v == n) {
            snprintf(out, size, "%s/%s", ticket_dir(), e->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

/* The capture into the ticket's folder, and a file by its number where it was; its name in the ticket */
static void move_capture(const char *folder, char *name, size_t size) {
    const char *from = capture_last_folder();
    name[0] = 0;
    if (!from || !from[0])
        return;
    char to[4300];
    for (int i = 1; i < 1000; i++) {
        if (i == 1)
            snprintf(name, size, "capture-%04d", capture_last_number());
        else
            snprintf(name, size, "capture-%04d-%d", capture_last_number(), i);
        snprintf(to, sizeof to, "%s/%s", folder, name);
        struct stat st;
        if (stat(to, &st) != 0)
            break;
    }
    if (rename(from, to) != 0) {        /* (another disk: it stays, and the ticket says where) */
        fprintf(stderr, "ticket: capture not moved (%s): %s\n", strerror(errno), from);
        snprintf(name, size, "%s", from);
        return;
    }
    FILE *f = fopen(from, "w");
    if (f) {
        fprintf(f, "%s\n", to);
        fclose(f);
    }
}

static int new_ticket(const char *body) {
    char slug[64], folder[4200], date[32], title[96], capture[256], ignore[8];
    int top, n = 0;
    find_ticket(-1, ignore, sizeof ignore, &top);
    /* the title: the note's start, whole words */
    snprintf(title, sizeof title, "%.72s", body);
    if (strlen(body) > 72 && strrchr(title, ' '))
        *strrchr(title, ' ') = 0;
    for (const char *c = title; *c && n < 48; c++)
        if (isalnum((unsigned char)*c))
            slug[n++] = (char)tolower((unsigned char)*c);
        else if (n && slug[n - 1] != '-')
            slug[n++] = '-';
    while (n && slug[n - 1] == '-')
        n--;
    slug[n] = 0;
    int number = top;
    do {
        number++;
        snprintf(folder, sizeof folder, "%s/%04d-%s", ticket_dir(), number, slug);
    } while (mkdir(folder, 0755) != 0 && errno == EEXIST);
    move_capture(folder, capture, sizeof capture);
    char path[4300];
    snprintf(path, sizeof path, "%s/ticket.md", folder);
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    today(date, sizeof date);
    fprintf(f, "# %04d %s\n\nstatus: open\ntype: bug\npriority: normal\nfiled: %s\ncommit:\ncheck:\n\n## What was seen\n\n%s\n\n## Findings\n",
            number, title, date, body);
    if (capture[0])
        fprintf(f, "\n## Evidence\n\n- `%s`\n", capture);
    fclose(f);
    return number;
}

static int add_to_ticket(int number, const char *body) {
    char folder[4200], path[4300], date[32], capture[256];
    int top;
    if (!find_ticket(number, folder, sizeof folder, &top))
        return 0;
    move_capture(folder, capture, sizeof capture);
    snprintf(path, sizeof path, "%s/ticket.md", folder);
    FILE *f = fopen(path, "a");
    if (!f)
        return 0;
    today(date, sizeof date);
    fprintf(f, "\n%s, from the game: %s\n", date, body);
    if (capture[0])
        fprintf(f, "\n- `%s`\n", capture);
    fclose(f);
    return number;
}

/* The note and the capture written: the ticket */
static void file_ticket(void) {
    char message[96];
    int number = 0;
    if (note[0] == '#' && isdigit((unsigned char)note[1])) {
        char *rest;
        long n = strtol(note + 1, &rest, 10);
        while (*rest == ' ')
            rest++;
        number = add_to_ticket((int)n, rest);
        if (number)
            snprintf(message, sizeof message, "added to ticket %d", number);
    }
    if (!number) {
        number = new_ticket(note);
        snprintf(message, sizeof message, number ? "ticket %d filed" : "ticket not filed: see the console", number);
    }
    fprintf(stderr, "%s\n", message);
    window_note(message);
}

static void wait_and_file(void) {
    extern void capture_wait(void);
    if (waiting) {
        capture_wait();
        waiting = 0;
        file_ticket();
    }
}

void ticket_key(void) {
    if (open_box || ending)
        return;
    static int hooked;
    if (!hooked) {
        hooked = 1;
        atexit(wait_and_file);          /* (the window closed just after Enter) */
    }
    if (waiting) {                      /* (the last one's capture still going: a moment) */
        window_note("still writing the last ticket");
        return;
    }
    was_held = inspect_held();
    inspect_hold(1);
    capture_request();
    open_box = hide_for_shot = 1;
    text[0] = 0;
    window_ticket_mode(1);
}

/* The box closed: the note, if there is one, kept for when the capture is written */
static void close_box(int keep) {
    open_box = 0;
    window_ticket_mode(0);
    if (keep && text[0]) {
        snprintf(note, sizeof note, "%s", text);
        waiting = 1;
    } else
        window_note(keep ? "no note: capture kept" : "capture kept, no ticket");
}

int ticket_event(const void *event) {
    const SDL_Event *e = event;
    if (ending)                         /* (the key that closed it, still down: ticket_poll) */
        return e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP;
    if (!open_box)
        return 0;
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        size_t have = strlen(text);
        snprintf(text + have, sizeof text - have, "%s", e->text.text);
        return 1;
    }
    if (e->type == SDL_EVENT_KEY_DOWN) {
        SDL_Scancode k = e->key.scancode;
        if (k == SDL_SCANCODE_BACKSPACE) {
            size_t n = strlen(text);
            while (n && ((unsigned char)text[n - 1] & 0xC0) == 0x80)   /* (a whole character) */
                n--;
            text[n ? n - 1 : 0] = 0;
        } else if (!e->key.repeat && (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER || k == SDL_SCANCODE_ESCAPE)) {
            close_box(k != SDL_SCANCODE_ESCAPE);
            ending = 1;
            ending_key = (int)k;
        }
        return 1;
    }
    return e->type == SDL_EVENT_KEY_UP || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP ||
           e->type == SDL_EVENT_MOUSE_WHEEL;
}

void ticket_test(unsigned long frame) {
    static int read;
    static unsigned long at;
    static int passes;
    static const char *given;
    if (!read) {
        read = 1;
        const char *e = getenv("MHFU_TEST_TICKET");
        if (e) {
            char *end;
            at = strtoul(e, &end, 10);
            if (*end == ',') {
                passes = (int)strtol(end + 1, &end, 10);
                if (*end == ',')
                    given = end + 1;
            }
        }
    }
    if (given && frame == at) {
        ticket_key();
        snprintf(text, sizeof text, "%s", given);
        test_passes = passes;
    }
}

void ticket_poll(void) {
    if (hide_for_shot && !capture_wants_shot())
        hide_for_shot = 0;
    if (ending && !SDL_GetKeyboardState(NULL)[ending_key]) {   /* (by its state: its release can be missed, the focus gone) */
        ending = 0;
        if (!was_held)
            inspect_hold(0);
    }
    if (test_passes >= 0 && open_box && test_passes-- == 0) {
        int esc = !strcmp(text, "ESC");
        close_box(!esc);
        if (!was_held)
            inspect_hold(0);
    }
    if (waiting && capture_idle()) {
        waiting = 0;
        file_ticket();
    }
}

int ticket_rects(float x0, float y0, float scale, HintRect *out, int max) {
    if (!open_box)
        return 0;
    if (hide_for_shot)
        return 0;                       /* (its capture is of the game, not of this) */
    int dot = (int)scale < 1 ? 1 : (int)scale;
    const int chars = 66;
    int w = chars * 6 + 8, h = 30;
    Pen p = {out, 0, max, dot, (int)(x0 + 240 * scale) - w * dot / 2, (int)(y0 + 28 * scale)};
    pen_box(&p, 0, 0, w, h, C_INK);
    pen_text(&p, 4, 4, "TICKET  ENTER SAVES  ESC CAPTURE ONLY  #12 ADDS TO 12", C_LIT);
    char shown[80];
    size_t n = strlen(text), start = n > (size_t)chars - 1 ? n - ((size_t)chars - 1) : 0, o = 0;
    for (size_t i = start; i < n && o < sizeof shown - 2; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c & 0xC0) == 0x80)
            continue;
        shown[o++] = c >= 0x80 ? '?' : (char)toupper(c);
    }
    shown[o++] = '_';
    shown[o] = 0;
    pen_text(&p, 4, 18, shown, C_WHITE);
    return p.n;
}

/* -- what the run prints ---------------------------------------------------------------------------------------------
 * With a window: stderr goes through a pipe, on to where it went, the last of it kept for a capture's console.txt. Not
 * headless, where tests read a run's output as it was, unless MHFU_CONSOLE=1. MHFU_CONSOLE=0: not at all. */
#define CONSOLE_KEEP (256 * 1024)
static char *console;
static size_t console_at, console_full;
static pthread_mutex_t console_lock = PTHREAD_MUTEX_INITIALIZER;
static int console_read_fd = -1, console_out_fd = -1;
static pthread_t console_thread;

static void *console_run(void *arg) {
    (void)arg;
    char buffer[4096];
    for (;;) {
        ssize_t n = read(console_read_fd, buffer, sizeof buffer);
        if (n <= 0)
            return NULL;
        if (write(console_out_fd, buffer, (size_t)n) < 0) {
            /* (nowhere to print: still kept) */
        }
        pthread_mutex_lock(&console_lock);
        for (ssize_t i = 0; i < n; i++) {
            console[console_at] = buffer[i];
            console_at = (console_at + 1) % CONSOLE_KEEP;
        }
        console_full = console_full + (size_t)n > CONSOLE_KEEP ? CONSOLE_KEEP : console_full + (size_t)n;
        pthread_mutex_unlock(&console_lock);
    }
}

static void console_end(void) {         /* (the last lines out, as the process ends) */
    fflush(stderr);
    dup2(console_out_fd, 2);
    pthread_join(console_thread, NULL);
}

void console_start(void) {
    const char *e = getenv("MHFU_CONSOLE");
    int fds[2];
    if ((e && atoi(e) == 0) || console || pipe(fds) != 0)
        return;
    console = malloc(CONSOLE_KEEP);
    console_out_fd = dup(2);
    console_read_fd = fds[0];
    if (!console || console_out_fd < 0 || dup2(fds[1], 2) < 0 ||
        pthread_create(&console_thread, NULL, console_run, NULL) != 0) {
        if (console_out_fd >= 0)
            dup2(console_out_fd, 2);
        free(console);
        console = NULL;
        close(fds[0]);
        close(fds[1]);
        return;
    }
    close(fds[1]);
    atexit(console_end);
}

char *console_copy(size_t *n) {
    *n = 0;
    if (!console)
        return NULL;
    pthread_mutex_lock(&console_lock);
    char *copy = malloc(console_full ? console_full : 1);
    if (copy) {
        size_t start = console_full == CONSOLE_KEEP ? console_at : 0;
        for (size_t i = 0; i < console_full; i++)
            copy[i] = console[(start + i) % CONSOLE_KEEP];
        *n = console_full;
    }
    pthread_mutex_unlock(&console_lock);
    return copy;
}
