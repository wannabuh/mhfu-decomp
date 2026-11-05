/* Frame pacing: what the display really did with the frames shown.
 *
 * The driver's VK_EXT_present_timing says, for each present, when its first pixel went out to the screen (or that it
 * never did: replaced by a later one). Those reports, read back without waiting after each present, are the only real
 * measure here: everything else about a frame is when we asked for it.
 *
 * MHFU_PRESENT_LOG=<file> writes a line for each frame shown (tools/pacing.py reads it):
 *   F <id> <issue> <due> <drawn> <flip> <queue> <pixel> <content> <game frame> <t x 10000> <ticks> <busy> <target>
 *     id       the present's number (0: no present came of it)
 *     issue    when the frame's drawing began, us on CLOCK_MONOTONIC from the log's start
 *     due      when it was due, less issue
 *     drawn    when its draws were queued, less issue
 *     flip     when vkQueuePresent returned, less issue
 *     queue    when the driver says the present's queue operations ended, less issue (0: not known)
 *     pixel    when the driver says its first pixel went out, less issue (0: not known; -1: never shown)
 *     content  the moment of the game it shows, us on the game's clock
 *     t        how far between the last two game frames
 *     ticks    game frames done since the frame shown before
 *     busy     how long the game's thread had been busy before this frame's drawing began (us): a tick in the way
 *     target   the refresh it was for, less issue (0: on a timer, not for any)
 *   T <when> <game frame> <game clock> : a game frame done
 * Off, none of this costs more than a branch.
 *
 * The lock (MHFU_PRESENT_MODE: lock, the default; old: as before). The refreshes are a grid, known from those same
 * reports, to within some tens of us. Each frame is made for one of them: it begins a fixed lead before it, and shows
 * the game as it will be at that refresh, less that lead (so the same step from each frame to the next, whether it
 * began on time or a tick held it up). The lead is what its drawing has been taking, a little to spare, and the wait
 * for the compositor, kept up to date; a frame that misses its refresh anyway is replaced by the next, on time (mailbox).
 * Without reports (another driver, no display), or on a variable refresh, or until they come: a timer at MHFU_FPS. A
 * display's grid that frames don't land on (a variable refresh the driver doesn't call so: not tried, no such screen
 * here) goes back to the timer the same way. */
#define _GNU_SOURCE 1
#include "pacing.h"
#include "perf.h"
#include "vk.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t wall_time_us(void);
uint64_t time_us(void);
uint64_t wall_since_idle_us(void);

int pacing_watch;
static int mode_old, lock_wanted, panel_on;

static FILE *log_file;
static uint64_t log_start;

/* -- the frames on their way --------------------------------------------------------------------------------------- */

#define RING 512
typedef struct {
    uint64_t id;
    uint64_t issue, due, target, drawn, flip, queue, pixel;
    int64_t content;
    unsigned long game_frame;
    float t;
    int ticks;
    uint64_t busy;
    int gone;                   /* never shown */
    int done;
} Rec;
static Rec ring[RING];
static uint64_t rec_next;       /* (the frames noted; rec_next % RING the next one's slot) */
static uint64_t written;        /* those before this are written and counted */
static Rec *current;            /* the frame being made, until its present */
static uint64_t present_id;
static int awaited;             /* reports asked for, not yet come */
static int ticks_since;

static PFN_vkGetPastPresentationTimingEXT get_past;
static PFN_vkSetSwapchainPresentTimingQueueSizeEXT set_queue_size;
static PFN_vkGetSwapchainTimingPropertiesEXT get_timing;
static PFN_vkGetSwapchainTimeDomainPropertiesEXT get_domains;
static uint64_t domain_id;
static int reports;             /* the swapchain has them */
static int domain_local;        /* in the present stages' own time, to be converted */
static int64_t stage_offset_ns[2];   /* CLOCK_MONOTONIC less that time: queue operations' end, first pixel out */
static PFN_vkGetCalibratedTimestampsKHR calibrate_fn;
static uint64_t refresh_ns;     /* the driver's */
static int refresh_variable;    /* the driver's word for it: no grid to lock to */
static double grid_at, grid_refresh;    /* a refresh's time (us), the time between them: 0 not known */
static uint64_t no_grid_until;          /* frames haven't been on it (a variable refresh?): not trusted until then */
static uint64_t last_report_at;         /* our clock, when a report last had a time in it */

/* -- the second's figures, for the panel ---------------------------------------------------------------------------- */

static struct {
    uint64_t from;
    unsigned long shown, replaced, repeats, late, steps;
    double step_sum, step_min, step_max;
} now_s;
static PacingStats last_s;
static uint64_t last_pixel;
static int64_t last_content;
static uint64_t refresh_seen_us;    /* the usual interval seen, if the driver says nothing */

static double lead_us;              /* (the schedule, below) */

static double refresh_us(void) {
    if (refresh_ns)
        return (double)refresh_ns / 1000;
    return (double)refresh_seen_us;
}

static void learn(const Rec *r);

static void count(const Rec *r) {
    learn(r);
    if (r->gone) {
        now_s.replaced++;
    } else if (r->pixel) {
        now_s.shown++;
        if (r->target && (double)r->pixel > (double)r->target + refresh_us() / 2)
            now_s.late++;
        if (last_pixel && r->pixel > last_pixel) {
            uint64_t gap = r->pixel - last_pixel;
            if (!refresh_ns && (!refresh_seen_us || gap < refresh_seen_us))
                refresh_seen_us = gap;
            double refresh = refresh_us();
            int n = refresh > 0 ? (int)((double)gap / refresh + 0.5) : 1;
            if (n < 1)
                n = 1;
            now_s.repeats += (unsigned long)(n - 1);
            double step = (double)(r->content - last_content) / n / 1000;
            if (!now_s.steps || step < now_s.step_min)
                now_s.step_min = step;
            if (!now_s.steps || step > now_s.step_max)
                now_s.step_max = step;
            now_s.step_sum += step;
            now_s.steps++;
        }
        last_pixel = r->pixel;
        last_content = r->content;
    } else
        return;
    uint64_t at = r->pixel ? r->pixel : r->issue;
    if (!now_s.from)
        now_s.from = at;
    if (at - now_s.from >= 1000000) {
        double refresh = refresh_us();
        last_s.have = 1;
        last_s.refresh_hz = refresh > 0 ? 1e6 / refresh : 0;
        last_s.shown = now_s.shown;
        last_s.replaced = now_s.replaced;
        last_s.repeats = now_s.repeats;
        last_s.late = now_s.late;
        last_s.step_ms = now_s.steps ? now_s.step_sum / (double)now_s.steps : 0;
        last_s.step_min_ms = now_s.step_min;
        last_s.step_max_ms = now_s.step_max;
        memset(&now_s, 0, sizeof now_s);
        now_s.from = at;
    }
}

void pacing_stats(PacingStats *out) {
    *out = last_s;
    out->lead_ms = lead_us / 1000;
}

/* -- the log -------------------------------------------------------------------------------------------------------- */

static long since(uint64_t at, uint64_t from) {
    return at ? (long)((int64_t)at - (int64_t)from) : 0;
}

static void write_rec(const Rec *r) {
    if (!log_file)
        return;
    long pixel = r->gone ? -1 : since(r->pixel, r->issue);
    fprintf(log_file, "F %llu %llu %ld %ld %ld %ld %ld %lld %lu %d %d %llu %ld\n", (unsigned long long)r->id,
            (unsigned long long)(r->issue - log_start), since(r->due, r->issue), since(r->drawn, r->issue),
            since(r->flip, r->issue), since(r->queue, r->issue), pixel, (long long)r->content, r->game_frame,
            (int)(r->t * 10000), r->ticks, (unsigned long long)r->busy, since(r->target, r->issue));
}

/* What has its report (in order), written and counted */
static void settle(int all) {
    while (written < rec_next) {
        Rec *r = &ring[written % RING];
        if (!r->done && !all && rec_next - written < RING / 2)
            break;
        if (r == current)
            break;
        write_rec(r);
        if (r->done)
            count(r);
        written++;
    }
}

static void flush_log(void) {
    settle(1);
    if (log_file)
        fflush(log_file);
}

static double next_target;          /* the refresh the next frame is for */
static double lead_want;            /* what the lead is going to */
static double extra_us;             /* the lead's margin, more for frames that missed anyway */
static uint32_t took[1024];         /* from when frames were due to the end of their queue operations, the last ones (us) */
static unsigned took_n;
static unsigned long since_lead, landed, landed_late, landed_early;
static int compositor_refreshes = 1;    /* how many refreshes it holds a frame from the one it took it at */

/* -- settings ----------------------------------------------------------------------------------------------------------- */


int pacing_old(void) {
    return mode_old;
}

void pacing_init(void) {
    const char *mode = getenv("MHFU_PRESENT_MODE");
    mode_old = mode && !strcmp(mode, "old");
    lock_wanted = !mode_old && (!mode || !strcmp(mode, "lock"));
    if (mode && !mode_old && !lock_wanted)
        fprintf(stderr, "MHFU_PRESENT_MODE: lock or old, not %s\n", mode);
    const char *path = getenv("MHFU_PRESENT_LOG");
    if (path && *path) {
        log_file = fopen(path, "w");
        if (!log_file)
            fprintf(stderr, "cannot write the present log %s\n", path);
        else {
            static char buffer[1 << 18];
            setvbuf(log_file, buffer, _IOFBF, sizeof buffer);
            log_start = wall_time_us();
            atexit(flush_log);
        }
    }
    pacing_watch_panel(panel_on);
}

void pacing_watch_panel(int on) {
    panel_on = on;
    pacing_watch = panel_on || log_file || (lock_wanted && reports);
}

VkPresentModeKHR pacing_present_mode(const VkPresentModeKHR *modes, uint32_t n) {
    VkPresentModeKHR want = VK_PRESENT_MODE_MAILBOX_KHR;
    const char *e = getenv("MHFU_PRESENT_SWAP");     /* (the swapchain's mode, to compare) */
    if (e && !strcmp(e, "fifo"))
        want = VK_PRESENT_MODE_FIFO_KHR;
    else if (e && !strcmp(e, "latest"))
        want = VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
    else if (e && !strcmp(e, "immediate"))
        want = VK_PRESENT_MODE_IMMEDIATE_KHR;
    for (uint32_t i = 0; i < n; i++)
        if (modes[i] == want)
            return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

/* -- the swapchain ------------------------------------------------------------------------------------------------------ */

/* The present stages' time against CLOCK_MONOTONIC, for each stage asked about */
static int calibrate(void) {
    static const VkPresentStageFlagsEXT stage[2] = {VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT,
                                                    VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT};
    for (int i = 0; i < 2; i++) {
        VkSwapchainCalibratedTimestampInfoEXT sw = {VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT, NULL,
                                                    vk.swapchain, stage[i], domain_id};
        VkCalibratedTimestampInfoKHR info[2] = {
            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, &sw, VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT},
            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, NULL, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR}};
        uint64_t ts[2], dev;
        VkResult r = calibrate_fn ? calibrate_fn(vk.device, 2, info, ts, &dev) : VK_ERROR_UNKNOWN;
        if (r != VK_SUCCESS)
            return 0;
        stage_offset_ns[i] = (int64_t)ts[1] - (int64_t)ts[0];
    }
    return 1;
}

void pacing_swapchain_created(void) {
    reports = 0;
    refresh_ns = 0;
    refresh_variable = 0;
    grid_refresh = 0;
    last_pixel = 0;
    awaited = 0;
    took_n = 0;
    since_lead = landed = landed_late = landed_early = 0;
    lead_want = extra_us = 0;
    compositor_refreshes = 1;
    if (!vk.present_timing)
        return;
    if (!get_past) {
        get_past = (PFN_vkGetPastPresentationTimingEXT)vkGetDeviceProcAddr(vk.device, "vkGetPastPresentationTimingEXT");
        set_queue_size = (PFN_vkSetSwapchainPresentTimingQueueSizeEXT)vkGetDeviceProcAddr(
            vk.device, "vkSetSwapchainPresentTimingQueueSizeEXT");
        get_timing = (PFN_vkGetSwapchainTimingPropertiesEXT)vkGetDeviceProcAddr(vk.device, "vkGetSwapchainTimingPropertiesEXT");
        get_domains = (PFN_vkGetSwapchainTimeDomainPropertiesEXT)vkGetDeviceProcAddr(
            vk.device, "vkGetSwapchainTimeDomainPropertiesEXT");
    }
    if (set_queue_size(vk.device, vk.swapchain, 64) != VK_SUCCESS)
        fprintf(stderr, "present timing: no queue for its reports\n");
    VkTimeDomainKHR domains[8];
    uint64_t ids[8];
    VkSwapchainTimeDomainPropertiesEXT d = {VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT, NULL, 8, domains, ids};
    VkResult r = get_domains(vk.device, vk.swapchain, &d, NULL);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        fprintf(stderr, "present timing: no time domains (%d)\n", r);
        return;
    }
    domain_local = 0;
    for (uint32_t i = 0; i < d.timeDomainCount; i++) {
        fprintf(stderr, "present timing: time domain %d, id %llu\n", domains[i], (unsigned long long)ids[i]);
        if (domains[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR)
            domain_id = ids[i], reports = 1, domain_local = 0;
        else if (domains[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT && !reports)
            domain_id = ids[i], reports = 1, domain_local = 1;
    }
    if (domain_local) {
        calibrate_fn = (PFN_vkGetCalibratedTimestampsKHR)vkGetDeviceProcAddr(vk.device, "vkGetCalibratedTimestampsKHR");
        if (!calibrate()) {
            fprintf(stderr, "present timing: its clock can't be tied to ours: not used\n");
            reports = 0;
        } else
            fprintf(stderr, "present timing: its clock is ours plus %lld, %lld ns\n", (long long)-stage_offset_ns[0],
                    (long long)-stage_offset_ns[1]);
    }
    pacing_watch_panel(panel_on);
}

/* The driver's refresh, when it has it (it may not, before the first frames are out) */
static void read_refresh(void) {
    if (!reports)
        return;
    VkSwapchainTimingPropertiesEXT p = {VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
    if (get_timing(vk.device, vk.swapchain, &p, NULL) == VK_SUCCESS && p.refreshDuration && p.refreshDuration != refresh_ns) {
        refresh_ns = p.refreshDuration;
        refresh_variable = p.refreshInterval == UINT64_MAX;
        grid_refresh = 0;           /* (and what was learned of frames at the rate before) */
        lead_us = lead_want = 0;
        took_n = 0;
        fprintf(stderr, "present timing: refresh %.3f us (%.5f Hz), interval %llu\n", (double)refresh_ns / 1000,
                1e9 / (double)refresh_ns, (unsigned long long)p.refreshInterval);
        if (log_file)
            fprintf(log_file, "# refresh_ns %llu interval %llu\n", (unsigned long long)refresh_ns,
                    (unsigned long long)p.refreshInterval);
    }
}

/* The refresh grid: a line through the reports' times, kept up by a little of each one's error */
static void grid_report(uint64_t pixel);

/* The reports come in: to the frames they are of */
static void read_reports(void) {
    enum { N = 16 };
    static VkPresentStageTimeEXT stages[N][2];
    static VkPastPresentationTimingEXT t[N];
    for (int again = 0; again < 8; again++) {
        for (int i = 0; i < N; i++) {
            t[i] = (VkPastPresentationTimingEXT){VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT};
            t[i].presentStageCount = 2;
            t[i].pPresentStages = stages[i];
        }
        VkPastPresentationTimingInfoEXT info = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT, NULL, 0, vk.swapchain};
        VkPastPresentationTimingPropertiesEXT props = {VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT};
        props.presentationTimingCount = N;
        props.pPresentationTimings = t;
        VkResult r = get_past(vk.device, &info, &props);
        if (r != VK_SUCCESS && r != VK_INCOMPLETE)
            return;
        for (uint32_t i = 0; i < props.presentationTimingCount; i++) {
            if (awaited)
                awaited--;
            /* (the frames go in order: one of those still kept in the ring) */
            for (uint64_t k = rec_next; k > written && rec_next - k < RING; k--) {
                Rec *f = &ring[(k - 1) % RING];
                if (f->id != t[i].presentId)
                    continue;
                f->done = 1;
                f->gone = 1;
                for (uint32_t s = 0; s < t[i].presentStageCount; s++) {
                    uint64_t at = stages[i][s].time;
                    if (stages[i][s].stage & VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT)
                        f->queue = at ? (uint64_t)((int64_t)at + (domain_local ? stage_offset_ns[0] : 0)) / 1000 : 0;
                    else if (stages[i][s].stage & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) {
                        f->pixel = at ? (uint64_t)((int64_t)at + (domain_local ? stage_offset_ns[1] : 0)) / 1000 : 0;
                        if (at) {
                            f->gone = 0;
                            grid_report(f->pixel);
                        }
                    }
                }
                break;
            }
        }
        if (r != VK_INCOMPLETE)
            break;
    }
}

/* -- the schedule ------------------------------------------------------------------------------------------------------- */

#define DEADLINE_MARGIN_US 700.0    /* to spare before the compositor takes what it has, about at the refresh (measured) */

static void grid_report(uint64_t pixel) {
    last_report_at = wall_time_us();
    double r = refresh_us();
    if (r <= 0)
        return;
    double p = (double)pixel;
    if (grid_refresh <= 0) {
        grid_refresh = r;
        grid_at = p;
        return;
    }
    double n = floor((p - grid_at) / grid_refresh + 0.5), error = p - (grid_at + n * grid_refresh);
    static int odd;
    static unsigned off_grid, reports_n;
    if (++reports_n >= 64) {        /* (a quarter of them off it, in a run: no steady refresh to lock to, as with VRR) */
        if (off_grid > 16) {
            no_grid_until = wall_time_us() + 5000000;
            fprintf(stderr, "present timing: %u of 64 frames off the refresh: on a timer for a while\n", off_grid);
        }
        reports_n = off_grid = 0;
    }
    if (fabs(error) > grid_refresh / 4) {
        off_grid++;
        if (++odd >= 16) {          /* (not a stray: the grid has moved) */
            grid_at = p;
            odd = 0;
        }
        return;
    }
    odd = 0;
    grid_at += n * grid_refresh + 0.03 * error;
    grid_refresh += 0.0002 * error;
}

/* The grid's refresh nearest a time */
static double grid_snap(double at) {
    return grid_at + floor((at - grid_at) / grid_refresh + 0.5) * grid_refresh;
}

static int refreshes_a_frame(int fps) {
    int n = (int)floor(1e6 / fps / grid_refresh + 0.5);
    return n < 1 ? 1 : n;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* A frame's report: how long its way to the compositor took, which refresh it reached */
static void learn(const Rec *r) {
    if (!lock_wanted || !r->target || grid_refresh <= 0)
        return;
    double R = grid_refresh;
    if (r->queue > r->due && r->queue - r->due < 100000) {
        took[took_n++ % 1024] = (uint32_t)(r->queue - r->due);
        since_lead++;
    }
    if (r->pixel) {
        double off = (double)r->pixel - (double)r->target;
        landed++;
        if (off > R / 2) {
            landed_late++;
            extra_us = extra_us + 100 > 2000 ? 2000 : extra_us + 100;
        } else if (off < -R / 2)
            landed_early++;
    }
    if (since_lead < 144 || took_n < 288)
        return;
    since_lead = 0;
    extra_us = extra_us > 50 ? extra_us - 50 : 0;
    /* most of a second's frames at another refresh than the one they were for: the compositor's wait isn't what was
     * taken (its own scheduling, the frame going straight to the screen) */
    if (landed >= 72 && landed_late * 10 > landed * 9 && compositor_refreshes < 3)
        compositor_refreshes++;
    else if (landed >= 72 && landed_early * 10 > landed * 9 && compositor_refreshes > 0)
        compositor_refreshes--;
    landed = landed_late = landed_early = 0;
    static uint32_t sorted[1024];
    unsigned n = took_n < 1024 ? took_n : 1024;
    memcpy(sorted, took, n * sizeof *sorted);
    qsort(sorted, n, sizeof *sorted, cmp_u32);
    double slowest = sorted[n - 1 - n / 1000], fastest = sorted[n / 100];
    lead_want = slowest + DEADLINE_MARGIN_US + extra_us + compositor_refreshes * R;
    /* all of them for one of the compositor's turns: a frame much quicker than the rest, begun too early, would reach the
     * turn before and show a refresh early. Those slower lose their refresh instead, the rarer */
    if (lead_want > fastest + (compositor_refreshes + 1) * R - 500)
        lead_want = fastest + (compositor_refreshes + 1) * R - 500;
}

int pacing_next(int fps, uint64_t wall_now, uint64_t *due, uint64_t *target, uint64_t *shows) {
    if (!lock_wanted || !vk.present_timing || refresh_variable || grid_refresh <= 0 || wall_now - last_report_at > 250000 ||
        wall_now < no_grid_until) {
        next_target = 0;
        lead_us = 0;
        return 0;
    }
    double R = grid_refresh, step = R * refreshes_a_frame(fps), now = (double)wall_now;
    if (lead_us <= 0)
        lead_us = lead_want > 0 ? lead_want : 2 * R;
    /* (to the lead wanted, by too little a step to see) */
    if (lead_want > 0)
        lead_us += lead_want > lead_us + 5 ? 5 : lead_want < lead_us - 5 ? -5 : lead_want - lead_us;
    if (next_target <= 0)
        next_target = grid_snap(now + lead_us + R);
    else
        next_target = grid_snap(next_target);
    /* held up by a whole frame or more: that one's gone, the next on time */
    while (next_target - lead_us + step <= now)
        next_target = grid_snap(next_target + step);
    *due = (uint64_t)(next_target - lead_us);
    *target = (uint64_t)next_target;
    *shows = (uint64_t)(next_target - lead_us);
    return 1;
}

void pacing_issued(int fps, uint64_t wall_now) {
    (void)wall_now;
    if (next_target > 0)
        next_target = grid_snap(next_target + grid_refresh * refreshes_a_frame(fps));
}

/* -- a frame ------------------------------------------------------------------------------------------------------------ */

void pacing_frame(uint64_t due, uint64_t target, uint64_t issue, int64_t content, unsigned long game_frame, float t) {
    if (!pacing_watch)
        return;
    settle(0);
    if (rec_next - written >= RING)     /* (never more than a few on their way, but the ring is not to overrun) */
        settle(1);
    current = &ring[rec_next++ % RING];
    memset(current, 0, sizeof *current);
    current->issue = issue;
    current->due = due;
    current->target = target;
    current->content = content;
    current->game_frame = game_frame;
    current->t = t;
    current->ticks = ticks_since;
    current->busy = wall_since_idle_us();
    ticks_since = 0;
}

void pacing_drawn(uint64_t wall_us) {
    if (current)
        current->drawn = wall_us;
}

void pacing_tick(unsigned long frame) {
    if (!pacing_watch)
        return;
    ticks_since++;
    if (log_file) {
        settle(0);
        uint64_t now = wall_time_us();
        fprintf(log_file, "T %llu %lu %llu\n", (unsigned long long)(now - log_start), frame,
                (unsigned long long)time_us());
        static uint64_t flushed;        /* (a crash loses no more than this) */
        if (now - flushed > 2000000) {
            flushed = now;
            fflush(log_file);
        }
    }
}

const void *pacing_present_chain(void) {
    if (!reports)
        return NULL;
    static uint64_t id;
    static VkPresentId2KHR present;
    static VkPresentTimingInfoEXT info;
    static VkPresentTimingsInfoEXT infos;
    id = ++present_id;
    present = (VkPresentId2KHR){VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, NULL, 1, &id};
    if (!pacing_watch || awaited >= 60)     /* (the reports' queue, of 64, is not to fill) */
        return &present;
    info = (VkPresentTimingInfoEXT){VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT};
    info.timeDomainId = domain_id;
    info.presentStageQueries = vk.present_stages & (VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT |
                                                    VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
    awaited++;
    infos = (VkPresentTimingsInfoEXT){VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, &present, 1, &info};
    if (current)
        current->id = id;
    return &infos;
}

void pacing_presented(VkResult r) {
    if (!pacing_watch)
        return;
    (void)r;
    if (current) {
        current->flip = wall_time_us();
        current->done = !reports;       /* (none to wait for) */
        current = NULL;
    }
    if (reports) {
        static unsigned every;
        if (!refresh_ns || ++every % 144 == 0)      /* (a window can go to another screen) */
            read_refresh();
        read_reports();
        static uint64_t calibrated;     /* (the two clocks might not stay together) */
        uint64_t now = wall_time_us();
        if (domain_local && now - calibrated > 2000000) {
            calibrated = now;
            calibrate();
        }
    }
    settle(0);
}
