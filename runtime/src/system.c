/* The rest of the system: display, controller, UMD, the utility dialogs (savedata, messages, keyboard, network),
 * and small libraries (language, clock, power, DMA, wireless). Savedata's work is in savedata.c. */
#include "hle.h"
#include "savestate.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -- display ------------------------------------------------------------------------------------------------------ */

uint32_t display_fb_addr, display_fb_width, display_fb_format;     /* for presenting the frame */

void hle_sceDisplaySetMode(Ctx *c) { RET(0); }

void hle_sceDisplaySetFrameBuf(Ctx *c) {
    if (ARG0) {
        display_fb_addr = ARG0;
        display_fb_width = ARG1;
        display_fb_format = ARG2;
    }
    HLE_LOG("sceDisplaySetFrameBuf(%08X, %u, %u, %u)", ARG0, ARG1, ARG2, ARG3);
    RET(0);
    present_frame();
}

/* -- controller ---------------------------------------------------------------------------------------------------- */

SAVED static uint32_t ctrl_cycle, ctrl_mode;
SAVED uint32_t ctrl_buttons;                 /* set by the frontend */
SAVED uint8_t ctrl_lx = 128, ctrl_ly = 128;

void hle_sceCtrlSetSamplingCycle(Ctx *c) {
    RET(ctrl_cycle);
    ctrl_cycle = ARG0;
}

void hle_sceCtrlSetSamplingMode(Ctx *c) {
    RET(ctrl_mode);
    ctrl_mode = ARG0;
}

/* (SceCtrlData *, count): timestamp, buttons, lx, ly, 6 reserved bytes */
void hle_sceCtrlReadBufferPositive(Ctx *c) {
    uint32_t n = ARG1 ? ARG1 : 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = ARG0 + 16 * i;
        SW(p, (uint32_t)time_us());
        SW(p + 4, ctrl_buttons);
        SB(p + 8, ctrl_lx);
        SB(p + 9, ctrl_ly);
        for (int k = 10; k < 16; k++)
            SB(p + (uint32_t)k, 0);
    }
    RET(n);
}

/* -- UMD ---------------------------------------------------------------------------------------------------------- */

void hle_sceUmdActivate(Ctx *c) { RET(0); }
void hle_sceUmdGetErrorStat(Ctx *c) { RET(0); }
void hle_sceUmdGetDriveStat(Ctx *c) { RET(0x32); }       /* present, ready, readable */

/* -- utility dialogs ---------------------------------------------------------------------------------------------- */

/* Status: 0 none, 1 initialising, 2 running, 3 finished, 4 shutting down. The dialog's result goes in its common
 * parameters at 0x1C. */
typedef struct { int status; uint32_t params; } Dialog;
SAVED static Dialog savedata, msgdialog, osk, netconf;

static void dialog_init(Dialog *d, uint32_t params) {
    d->status = 1;
    d->params = params;
}

/* Polling a dialog moves it along by itself as the PSP's does: from initialising to running, and from shutting down
 * to none */
static int dialog_status(Dialog *d) {
    int s = d->status;
    if (s == 4)
        d->status = 0;
    else if (s == 1)
        d->status = 2;
    return s;
}

static void dialog_update(Dialog *d, void (*work)(Dialog *)) {
    if (d->status == 1) {
        d->status = 2;
    } else if (d->status == 2) {
        if (work)
            work(d);
        d->status = 3;
    }
}

void savedata_run(uint32_t params);     /* savedata.c */

static void savedata_work(Dialog *d) { savedata_run(d->params); }

void hle_sceUtilitySavedataInitStart(Ctx *c) {
    dialog_init(&savedata, ARG0);
    HLE_LOG("savedata: mode %u, game %.13s, save %.20s, file %.13s, buffer %08X size %X", LW(ARG0 + 0x30),
            hle_string(c, ARG0 + 0x3C), hle_string(c, ARG0 + 0x4C), hle_string(c, ARG0 + 0x64), LW(ARG0 + 0x74),
            LW(ARG0 + 0x78));
    RET(0);
}

/* The savedata utility works in the background: its status advances as the game polls it */
void hle_sceUtilitySavedataGetStatus(Ctx *c) {
    int s = dialog_status(&savedata);
    if (s == 1 || s == 2)
        dialog_update(&savedata, savedata_work);
    RET(s);
}

void hle_sceUtilitySavedataUpdate(Ctx *c) {
    dialog_update(&savedata, savedata_work);
    RET(0);
}

void hle_sceUtilitySavedataShutdownStart(Ctx *c) {
    savedata.status = 4;
    RET(0);
}

/* Message dialogs: answered at once - "yes" if it asks yes or no (option 0x10), else just closed */
static void msgdialog_work(Dialog *d) {
    SW(d->params + 0x1C, 0);
    SW(d->params + 0x240, (LW(d->params + 0x23C) & 0x10) ? 1 : 0);
}

void hle_sceUtilityMsgDialogInitStart(Ctx *c) {
    dialog_init(&msgdialog, ARG0);
    HLE_WARN("message dialog: size %X mode %u error %08X options %X text \"%s\"", LW(ARG0), LW(ARG0 + 0x34),
             LW(ARG0 + 0x38), LW(ARG0 + 0x23C), hle_string(c, ARG0 + 0x3C));
    RET(0);
}

void hle_sceUtilityMsgDialogGetStatus(Ctx *c) { RET(dialog_status(&msgdialog)); }

void hle_sceUtilityMsgDialogUpdate(Ctx *c) {
    dialog_update(&msgdialog, msgdialog_work);
    RET(0);
}

void hle_sceUtilityMsgDialogShutdownStart(Ctx *c) {
    msgdialog.status = 4;
    RET(0);
}

/* The on-screen keyboard: answered at once with MHFU_NAME (default "Hunter") - for now the only text the game asks
 * for is the hunter's name. SceUtilityOskParams: the common part, the number of fields (0x30), the fields (0x34);
 * SceUtilityOskData: ..., description (28), initial text (32), output length (36), output text (40, UTF-16),
 * result (44: 0 unchanged, 1 cancelled, 2 changed), output limit (48) */
static void osk_work(Dialog *d) {
    const char *text = getenv("MHFU_NAME");
    if (!text)
        text = "Hunter";
    uint32_t n = LW(d->params + 0x30), data = LW(d->params + 0x34);
    for (uint32_t i = 0; i < n && i < 4; i++) {
        uint32_t f = data + 52 * i, out = LW(f + 40), len = LW(f + 36), limit = LW(f + 48);
        uint32_t max = len ? len : limit ? limit : 16;
        uint32_t k = 0;
        for (; text[k] && k + 1 < max; k++)
            SH(out + 2 * k, (uint8_t)text[k]);
        SH(out + 2 * k, 0);
        SW(f + 44, 2);
    }
    SW(d->params + 0x1C, 0);
}

void hle_sceUtilityOskInitStart(Ctx *c) {
    dialog_init(&osk, ARG0);
    RET(0);
}

void hle_sceUtilityOskGetStatus(Ctx *c) { RET(dialog_status(&osk)); }

void hle_sceUtilityOskUpdate(Ctx *c) {
    dialog_update(&osk, osk_work);
    RET(0);
}

void hle_sceUtilityOskShutdownStart(Ctx *c) {
    osk.status = 4;
    RET(0);
}

/* Network configuration: cancelled - there is no network */
static void netconf_work(Dialog *d) { SW(d->params + 0x1C, 1); }

void hle_sceUtilityNetconfInitStart(Ctx *c) {
    dialog_init(&netconf, ARG0);
    RET(0);
}

void hle_sceUtilityNetconfGetStatus(Ctx *c) { RET(dialog_status(&netconf)); }

void hle_sceUtilityNetconfUpdate(Ctx *c) {
    dialog_update(&netconf, netconf_work);
    RET(0);
}

void hle_sceUtilityNetconfShutdownStart(Ctx *c) {
    netconf.status = 4;
    RET(0);
}

void hle_sceUtilityLoadModule(Ctx *c) { RET(0); }
void hle_sceUtilityUnloadModule(Ctx *c) { RET(0); }

/* System settings: English, X to confirm (as a US PSP), 24-hour clock, UTC */
void hle_sceUtilityGetSystemParamInt(Ctx *c) {
    uint32_t v;
    switch (ARG0) {
    case 8: v = 1; break;           /* language: English */
    case 9: v = 1; break;           /* confirm button: X */
    default: v = 0; break;
    }
    SW(ARG1, v);
    RET(0);
}

/* -- small libraries ---------------------------------------------------------------------------------------------- */

void hle_sceImposeSetLanguageMode(Ctx *c) { RET(0); }
void hle_scePowerRegisterCallback(Ctx *c) { RET(0); }

uint64_t epoch_now_us(void);

void hle_sceRtcGetCurrentClockLocalTime(Ctx *c) {   /* ScePspDateTime: year, month, day, hour, minute, second (u16), us (u32) */
    time_t now = (time_t)(epoch_now_us() / 1000000u);
    struct tm tm;
    localtime_r(&now, &tm);
    SH(ARG0, tm.tm_year + 1900);
    SH(ARG0 + 2, tm.tm_mon + 1);
    SH(ARG0 + 4, tm.tm_mday);
    SH(ARG0 + 6, tm.tm_hour);
    SH(ARG0 + 8, tm.tm_min);
    SH(ARG0 + 10, tm.tm_sec);
    SW(ARG0 + 12, 0);
    RET(0);
}

void hle_sceOpenPSIDGetOpenPSID(Ctx *c) {
    for (int i = 0; i < 16; i++)
        SB(ARG0 + (uint32_t)i, 0x10 + i);
    RET(0);
}

void hle_sceDmacMemcpy(Ctx *c) {
    memmove(PTR(ARG0), PTR(ARG1), ARG2);
    RET(0);
}

void hle_sceWlanGetSwitchState(Ctx *c) { RET(0); }

void hle_sceWlanGetEtherAddr(Ctx *c) {
    for (int i = 0; i < 6; i++)
        SB(ARG0 + (uint32_t)i, 0x10 + i);
    RET(0);
}
