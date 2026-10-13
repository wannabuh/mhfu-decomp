/* Mouse camera: the mouse turns the game's camera, by writing its yaw before each game frame.
 *
 * The game's scene (at *0x089CBD04) has six camera views of 0x1A0 bytes from +0xB0, the one in use numbered by the
 * byte at +0xA8D (0 in a quest, 2 in the village). A view's eye is at +0x00 and its target at +0x10; at +0x120 are
 * two 16-bit angles (65536 a turn), the yaw it is turning to and, at +0x122, the yaw it has. The camera eases from
 * one to the other, so turning both turns it at once. ← on the D-pad turns it left, raising the yaw.
 *
 * MHFU_MOUSE_SENS: degrees a mouse count turns (default 0.15). */
#include "hle.h"

#include <stdlib.h>

void window_mouse(float *dx, float *dy);

#define SCENE_PTR 0x089CBD04u

static int in_ram(uint32_t a) {
    return a >= 0x08800000u && a < 0x0A000000u;
}

void camera_update(void) {
    static float sens = -1, left;
    if (sens < 0) {
        const char *e = getenv("MHFU_MOUSE_SENS");
        sens = e ? (float)atof(e) : 0.15f;
    }
    float dx, dy;
    window_mouse(&dx, &dy);
    (void)dy;
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
    if (!turn)
        return;
    left -= (float)turn;
    uint16_t *yaw = (uint16_t *)MEM(view + 0x120);
    yaw[0] = (uint16_t)(yaw[0] + turn);
    yaw[1] = (uint16_t)(yaw[1] + turn);
}
