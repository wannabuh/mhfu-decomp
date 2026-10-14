"""Changes to the game's code, made as it is translated: (module, address) -> (the instruction there, as disassembled,
checked so that a patch never lands on something else; the C to run instead; why).

The native frame rate: the game counts time in its frames (30 a second). With MHFU_TICK_HZ=N it runs N frames a
second and rt_time_scale is 30 / N; what advances by so much a frame is patched to advance by that much times
rt_time_scale (1 at the game's own rate, so the patches change nothing then). docs/native.md keeps the list.
"""

# The motion player (EBOOT 088637C4): each layer's frame (+0x10) advances by its speed (+0x14) a game frame - 2.0 for
# the 60 frames a second the motions are made at. The speed is loaded in four places; each load is scaled.
MOTION = "the motion player: a layer's speed (frames a game frame), times the time scale"


def scaled_load(f, base, off):
    """lwc1 $f<f>, off($<base>), the value times the time scale"""
    return f"c->f[{f}].u = LW((c->r[{base}] + {off:#x}u)); c->f[{f}].f *= rt_time_scale;"


PATCHES = {
    ("EBOOT", 0x088638C0): ("lwc1 $f1, 0x14($s1)", scaled_load(1, 17, 0x14), MOTION),
    ("EBOOT", 0x088638D4): ("lwc1 $f0, 0x14($s1)", scaled_load(0, 17, 0x14), MOTION),
    ("EBOOT", 0x08863978): ("lwc1 $f1, 0x14($s1)", scaled_load(1, 17, 0x14), MOTION),
    ("EBOOT", 0x08863994): ("lwc1 $f1, 0x14($s1)", scaled_load(1, 17, 0x14), MOTION),
}
