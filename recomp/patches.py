"""Changes to the game's code, made as it is translated: (module, address) -> (the instruction there, as disassembled,
checked so that a patch never lands on something else; the C to run instead; why).

The native frame rate: the game counts time in its frames (30 a second). With MHFU_TICK_HZ=N it runs N frames a
second and rt_time_scale is 30 / N; what advances by so much a frame is patched to advance by that much times
rt_time_scale. A count of frames in an integer cannot go in fractions: it counts only on the frames where a frame at
30 a second would fall (rt_tick30, 1 or 0) - the same time, in steps of a 30th. At the game's own rate the scale is 1
and every frame a 30th, so the patches change nothing then. docs/native.md keeps the list.
"""

# The motion player (EBOOT 088637C4): each layer's frame (+0x10) advances by its speed (+0x14) a game frame - 2.0 for
# the 60 frames a second the motions are made at. The speed is loaded in four places; each load is scaled.
MOTION = "the motion player: a layer's speed (frames a game frame), times the time scale"
BLEND = "the motion player: a blend's weight step times the time scale, its count of frames at 30 a second"

# The hunt's camera (EBOOT 08886E50, view at $s4 - 0xA0, its yaw to turn to at +0x80) turns itself where the scenery is
# in the way: it casts rays between the eye and the hunter; one hit, it turns 0x400 a frame (5.6 degrees) until it is
# clear; the line of sight lost (+0x89), it swings an eighth of the way behind the hunter a frame. With the mouse
# turning the camera (rt_camera_free) those turns are left out: the camera goes where the mouse puts it.
CAMERA = "the hunt's camera: its own turns away from the scenery, left out while the mouse has the camera"

# The test of a model's sphere against the camera's view (EBOOT 088160F0: a0 the scene, a1 the centre, f12 the radius;
# 1 if it is to be drawn): what is drawn casts the renderer's shadows, so with them a model is drawn when within
# rt_cull_margin (units) of the view - the shadows of what is just out of view do not come and go as the camera turns.
CULL = "the view test of a model's sphere: the sphere larger by rt_cull_margin (the shadows of what is just out of view)"


def scaled_load(f, base, off):
    """lwc1 $f<f>, off($<base>), the value times the time scale"""
    return f"c->f[{f}].u = LW((c->r[{base}] + {off:#x}u)); c->f[{f}].f *= rt_time_scale;"


PATCHES = {
    ("EBOOT", 0x088638C0): ("lwc1 $f1, 0x14($s1)", scaled_load(1, 17, 0x14), MOTION),
    ("EBOOT", 0x088638D4): ("lwc1 $f0, 0x14($s1)", scaled_load(0, 17, 0x14), MOTION),
    ("EBOOT", 0x08863978): ("lwc1 $f1, 0x14($s1)", scaled_load(1, 17, 0x14), MOTION),
    ("EBOOT", 0x08863994): ("lwc1 $f1, 0x14($s1)", scaled_load(1, 17, 0x14), MOTION),
    # a blend between motions: its weight (+0x34) steps by +0x24 a game frame, while its count of frames (+0x30) runs out
    ("EBOOT", 0x08863894): ("add.s $f1, $f1, $f2", "c->f[1].f = c->f[1].f + c->f[2].f * rt_time_scale;", BLEND),
    ("EBOOT", 0x088638A0): ("addiu $v0, $v0, -0x1", "c->r[2] = c->r[2] - (uint32_t)rt_tick30;", BLEND),
    ("EBOOT", 0x08887608): ("sh $v0, 0x80($s4)", "if (!rt_camera_free) SH((c->r[20] + 0x80u), c->r[2]);", CAMERA),
    ("EBOOT", 0x08887C58): ("sh $v0, 0x80($s4)", "if (!rt_camera_free) SH((c->r[20] + 0x80u), c->r[2]);", CAMERA),
    ("EBOOT", 0x088160F0): ("lbu $v0, 0xAAA($a0)", "c->r[2] = LBU((c->r[4] + 0xAAAu)); c->f[12].f += rt_cull_margin;", CULL),
}
