/* Frame pacing: what the display really did with the frames shown.
 *
 * The driver's VK_EXT_present_timing says, for each present, when its first pixel went out to the screen (or that it
 * never did: replaced by a later one). Those reports, read back without waiting after each present, are the only real
 * measure here: everything else about a frame is when we asked for it.
 *
 * MHFU_PRESENT_LOG=<file> writes a line for each frame shown (tools/pacing.py reads it):
 *   F <id> <issue> <due> <drawn> <flip> <queue> <pixel> <content> <game frame> <t x 10000> <ticks> <busy>
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
 *   T <when> <game frame> <game clock> : a game frame done
 * Off, none of this costs more than a branch. */
#define _GNU_SOURCE 1
#include "pacing.h"
#include "perf.h"
#include "vk.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t wall_time_us(void);
uint64_t time_us(void);
uint64_t wall_since_idle_us(void);

int pacing_watch;

static FILE *log_file;
static uint64_t log_start;

/* -- the frames on their way --------------------------------------------------------------------------------------- */

#define RING 512
typedef struct {
    uint64_t id;
    uint64_t issue, due, drawn, flip, queue, pixel;
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

/* -- the second's figures, for the panel ---------------------------------------------------------------------------- */

static struct {
    uint64_t from;
    unsigned long shown, replaced, repeats, steps;
    double step_sum, step_min, step_max;
} now_s;
static PacingStats last_s;
static uint64_t last_pixel;
static int64_t last_content;
static uint64_t refresh_seen_us;    /* the usual interval seen, if the driver says nothing */

static double refresh_us(void) {
    if (refresh_ns)
        return (double)refresh_ns / 1000;
    return (double)refresh_seen_us;
}

static void count(const Rec *r) {
    if (r->gone) {
        now_s.replaced++;
    } else if (r->pixel) {
        now_s.shown++;
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
        last_s.step_ms = now_s.steps ? now_s.step_sum / (double)now_s.steps : 0;
        last_s.step_min_ms = now_s.step_min;
        last_s.step_max_ms = now_s.step_max;
        memset(&now_s, 0, sizeof now_s);
        now_s.from = at;
    }
}

void pacing_stats(PacingStats *out) {
    *out = last_s;
}

/* -- the log -------------------------------------------------------------------------------------------------------- */

static long since(uint64_t at, uint64_t from) {
    return at ? (long)((int64_t)at - (int64_t)from) : 0;
}

static void write_rec(const Rec *r) {
    if (!log_file)
        return;
    long pixel = r->gone ? -1 : since(r->pixel, r->issue);
    fprintf(log_file, "F %llu %llu %ld %ld %ld %ld %ld %lld %lu %d %d %llu\n", (unsigned long long)r->id,
            (unsigned long long)(r->issue - log_start), since(r->due, r->issue), since(r->drawn, r->issue),
            since(r->flip, r->issue), since(r->queue, r->issue), pixel, (long long)r->content, r->game_frame,
            (int)(r->t * 10000), r->ticks, (unsigned long long)r->busy);
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

/* -- settings ----------------------------------------------------------------------------------------------------------- */

void pacing_init(void) {
    const char *path = getenv("MHFU_PRESENT_LOG");
    if (path && *path) {
        log_file = fopen(path, "w");
        if (!log_file)
            fprintf(stderr, "cannot write the present log %s\n", path);
        else {
            static char buffer[1 << 18];
            setvbuf(log_file, buffer, _IOFBF, sizeof buffer);
            log_start = wall_time_us();
            pacing_watch = 1;
            atexit(flush_log);
        }
    }
}

void pacing_watch_panel(int on) {
    pacing_watch = on || log_file;
}

VkPresentModeKHR pacing_present_mode(const VkPresentModeKHR *modes, uint32_t n) {
    VkPresentModeKHR want = VK_PRESENT_MODE_MAILBOX_KHR;
    const char *e = getenv("MHFU_PRESENT_MODE");
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
    last_pixel = 0;
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
}

/* The driver's refresh, when it has it (it may not, before the first frames are out) */
static void read_refresh(void) {
    if (refresh_ns || !reports)
        return;
    VkSwapchainTimingPropertiesEXT p = {VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT};
    if (get_timing(vk.device, vk.swapchain, &p, NULL) == VK_SUCCESS && p.refreshDuration) {
        refresh_ns = p.refreshDuration;
        fprintf(stderr, "present timing: refresh %.3f us (%.5f Hz), interval %llu\n", (double)refresh_ns / 1000,
                1e9 / (double)refresh_ns, (unsigned long long)p.refreshInterval);
        if (log_file)
            fprintf(log_file, "# refresh_ns %llu interval %llu\n", (unsigned long long)refresh_ns,
                    (unsigned long long)p.refreshInterval);
    }
}

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
                        if (at)
                            f->gone = 0;
                    }
                }
                break;
            }
        }
        if (r != VK_INCOMPLETE)
            break;
    }
}

/* -- a frame ------------------------------------------------------------------------------------------------------------ */

void pacing_frame(uint64_t due, uint64_t issue, int64_t content, unsigned long game_frame, float t) {
    if (!pacing_watch)
        return;
    settle(0);
    if (rec_next - written >= RING)     /* (never more than a few on their way, but the ring is not to overrun) */
        settle(1);
    current = &ring[rec_next++ % RING];
    memset(current, 0, sizeof *current);
    current->issue = issue;
    current->due = due;
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
        fprintf(log_file, "T %llu %lu %llu\n", (unsigned long long)(wall_time_us() - log_start), frame,
                (unsigned long long)time_us());
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
    if (!pacing_watch)
        return &present;
    info = (VkPresentTimingInfoEXT){VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT};
    info.timeDomainId = domain_id;
    info.presentStageQueries = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
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
