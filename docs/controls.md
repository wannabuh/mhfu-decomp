# Controls

`runtime/src/controls.c` binds the keyboard, the mouse and a gamepad to actions, and presses the PSP buttons the game
wants for each (README: Controls). What it presses goes into `ctrl_buttons` like any button, so recordings hold the
buttons: they replay the same whatever the bindings.

## What the game wants

Found with RAM snapshots (`MHFU_SNAPSHOT`) around presses appended to `quest.rec` at the camp's supply box:

- **The hunter**: the local player's object, ticked at `EBOOT 088DB474` (native.c's `rt_local_player`); in a hunt its
  vtable is `089BB3CC`. The village's player is another object: there the actions do as with the weapon away.
- **Weapon drawn**: hunter `+0x410` bit `0x8` (1 away, 9 drawn). It clears as soon as the weapon starts going away;
  the action's kind (`+0x685`) stays non-zero until it is away (0: free to move).
- **Weapon class**: hunter `+0x1E8`, 0-10 (`game_task 09A92228` switches on it).
- **Action**: `+0x684` the action, `+0x685` its kind (0 walking or standing, 3 drawing / putting away, ...).
  Action numbers are each weapon class's own (the class above's put-away is `0x3EB`).
- **Selected item**: hunter `+0x648`, the pouch slot shown on the item bar.
- **Item bar**: L held for at least 10 30ths opens the bar's choice; with L still held, □ goes to the next item and ○
  to the one before, a step per press. L for 8 or 9 30ths before □ does nothing.
- **Interact**: ○ with the weapon away (opens the supply box; talks, gathers, carves). With the weapon out ○ attacks.
- **Use item**: □ with the weapon away; with it out, □ puts it away (sword and shield: uses the item drawn).
- **Roll**: × (action 7, then 8). △ + ○ + × together does the same as × alone (no kick).

## Button hints

`runtime/src/glyphs.c`. The PSP's buttons on screen are sprites of the HUD's atlas (texture `0x8001`; the sprite
table at `089392C8`, 12 bytes each: u, v, width, height, texture): 50 ○, 51 ×, 52 △, 53 □, 54 L, 55 R, 57 START,
58 SELECT, 59 the D-pad. Two of the game's sprite drawers draw them: `EBOOT 0882AF70` (t1 the sprite, a3 x t0 its
size) and `0882A9DC` (a3 the sprite, at its own size: the item bar's). About a dozen more drawers read the same table
(scaled, coloured, turned) - none seen drawing a button yet. Seen so far (`MHFU_GLYPH_LOG=1`):

| From | Sprite | What |
|------|--------|------|
| `08845D98`, `08845DB4` | □, L | the item bar (use item; hold L to choose) |
| `08846898`, `088468D0` | □, ○ | the item bar's choice open (next, previous) |
| `09A8DA70` (lobby_task) | □ | a lobby menu |
| `08838E24` | □ | (at 16,16) |

A hint for an action with an input on the device in use is left out of the game's drawing (the drawer returns at
once: only its display list changes), and the renderer draws the input's picture over its place, at the window's
resolution, as cleared rectangles after the picture is presented (render.c `draw_hints`). The 2D is drawn at 30ths:
the hints noted in a 30th's frame are shown until the next 30th's. Hints drawn as text (the font's own button
letters) are not covered yet.

## Controls screen

`runtime/src/overlay.c`, F1: drawn the same way over the presented picture; while it is open `present_frame` polls
the window and presents the last frame again, the game's clock not running (the pacing starts again afterwards:
thread.c resets it after a stall). Not with the software renderer (nothing is drawn over its picture).

## Testing

`MHFU_TEST_DEVICE=1` shows the gamepad's hints, `MHFU_TEST_OVERLAY=set,row` draws the controls screen (nothing
paused); both are seen in frames dumped as presented (`MHFU_FPS=60 MHFU_DUMP_AT=N MHFU_DUMP_PRESENT=960x544`).
`MHFU_TEST_ACTION="frame,action[,frames];..."` holds an action headless (after a replay ends: during one the
recording's buttons are the game's). `MHFU_CONTROLS_LOG=1` prints the buttons as they change, with the hunter's state.
For example, `quest.rec` cut as for the supply box, items taken and the box closed, then
`MHFU_TEST_ACTION=3700,item_next,1` moves the item bar on one (`+0x648` 5 to 6), at 30 and at `MHFU_TICK_HZ=60`.
