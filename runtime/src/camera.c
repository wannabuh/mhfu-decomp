/* Mouse camera: the mouse turns the game's camera, by writing its yaw before each game frame.
 *
 * The game's scene (at *0x089CBD04) has six camera views of 0x1A0 bytes from +0xB0, the one in use numbered by the
 * byte at +0xA8D (0 in a quest, 2 in the village). A view's eye is at +0x00 and its target at +0x10; at +0x120 are
 * two 16-bit angles (65536 a turn), the yaw it is turning to and, at +0x122, the yaw it has. The camera eases from
 * one to the other, so turning both turns it at once. ← on the D-pad turns it left, raising the yaw.
 *
 * Its height is a preset, pointed to from +0x110 (↑ and ↓ step through a table of them): floats at +4, the eye's height
 * over the hunter, +8 its distance and +0x10 the height it looks at. Moving the mouse up and down pitches the preset in
 * use: its eye moves around the point it looks at, keeping its distance, from 20 degrees below to 80 above; the camera
 * eases to it.
 *
 * MHFU_MOUSE_SENS: degrees a mouse count turns (default 0.15). */
#include "hle.h"

#include <math.h>
#include <stdlib.h>

void window_mouse(float *dx, float *dy);

#define SCENE_PTR 0x089CBD04u

static int in_ram(uint32_t a) {
    return a >= 0x08800000u && a < 0x0A000000u;
}

/* The presets the mouse has pitched: the eye's distance from where it looks, its angle above it, and what was last
 * written (if the game has written something else since, it is the game's preset again) */
static struct { uint32_t addr; float radius, angle, height, distance; } pitched[16];

static void pitch(uint32_t preset, float by) {
    float *height = (float *)MEM(preset + 4), *distance = (float *)MEM(preset + 8), look = *(float *)MEM(preset + 0x10);
    if (!(*distance > 0 && *distance < 5000))
        return;
    int i = 0;
    while (i < 16 && pitched[i].addr && pitched[i].addr != preset)
        i++;
    if (i == 16)
        i = 0;
    if (pitched[i].addr != preset || pitched[i].height != *height || pitched[i].distance != *distance) {
        float up = *height - look;
        pitched[i].addr = preset;
        pitched[i].radius = sqrtf(up * up + *distance * *distance);
        pitched[i].angle = atan2f(up, *distance);
    }
    float a = pitched[i].angle + by, lo = -20 * 3.14159265f / 180, hi = 80 * 3.14159265f / 180;
    a = a < lo ? lo : a > hi ? hi : a;
    pitched[i].angle = a;
    *height = pitched[i].height = look + pitched[i].radius * sinf(a);
    *distance = pitched[i].distance = pitched[i].radius * cosf(a);
}

void camera_update(void) {
    static float sens = -1, left;
    if (sens < 0) {
        const char *e = getenv("MHFU_MOUSE_SENS");
        sens = e ? (float)atof(e) : 0.15f;
    }
    float dx, dy;
    window_mouse(&dx, &dy);
    uint32_t scene = *(uint32_t *)MEM(SCENE_PTR);
    if (!in_ram(scene))
        return;
    uint8_t view_no = *MEM(scene + 0xA8D);
    if (view_no >= 6)
        return;
    uint32_t view = scene + 0xB0 + view_no * 0x1A0u;
    /* moving the mouse right turns the camera right: the yaw goes down */
    left += -dx * sens * 65536.0f / 360.0f;
    int turn = (int)left;
    if (turn) {
        left -= (float)turn;
        uint16_t *yaw = (uint16_t *)MEM(view + 0x120);
        yaw[0] = (uint16_t)(yaw[0] + turn);
        yaw[1] = (uint16_t)(yaw[1] + turn);
    }
    /* the pitch: moving the mouse up looks up, the eye going down around the hunter at the same distance */
    uint32_t preset = *(uint32_t *)MEM(view + 0x110);
    if (dy != 0 && in_ram(preset))
        pitch(preset, dy * sens * 3.14159265f / 180.0f);
}
