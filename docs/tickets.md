# Tickets

Everything seen wrong or wanted: bugs, ideas and impressions. `tools/tickets.py` lists and changes them; its help has every command.

    tools/tickets.py            # what's open, highest priority first
    tools/tickets.py check      # ready for the user to check
    tools/tickets.py show 12

**From the game: F6.** It holds the game, captures the frame shown (a frame gone back to, too) and opens a note box; the mouse is let go and the keys type. Enter files a ticket, and goes on once the key is let go, unless it was held before F6. `#12 happens on Khezu too` adds to ticket 12; a number with no ticket makes a new one, the note whole. Esc, or Enter with no note, keeps the capture alone, in the captures' folder. New tickets are `open`, `bug`, `normal`. With the software renderer F6 only captures. `MHFU_TEST_TICKET="frame,passes,note"` does it headless (`ESC` for Esc).

**Where.** One folder each, `~/.local/share/mhfu-recomp/tickets/0012-title/` (`$MHFU_TICKETDIR`, or under `$XDG_DATA_HOME`): `ticket.md` and its evidence together, outside every checkout and not under version control, so that it's the same from any of them and `git clean` can't touch it. There's no backup but what the machine has. Tests point `MHFU_TICKETDIR` elsewhere.

**Captures** hold the save state, screenshot, last 10 s of inputs and events, settings, and now the last of what the run printed (`console.txt`, with a window, `MHFU_CONSOLE=0` not) and of the hit overlay's and stall recorder's logs, if they're on. `attach 12 0010` moves a capture in, so nothing cleans it up, and leaves a file named `0010` in the captures' folder: the game counts the next number from it, and `tools/capture.py 10` follows it. Files are copied. Evidence is kept for good: no cleanup until space is a problem.

**Status.**
- `open`, `in-progress`
- `fix-ready`: built, with a `check=` saying what to look at, and in the play build. Not done until played
- `seems-original`: looks like the game's own behaviour, for the user to accept or reject
- `later`: kept open, not worked on (priority `later` means this)
- `closed`: only at the user's word. `set` refuses it

**Priority.** `high` or `normal`, optional on filing; Claude suggests one by the roadmap's order, and the user can overrule it. Work starts when the user says so. Builds go on `auto/` branches, and merging into `main` takes the user's word.

**Commits** name the ticket (`#12`), and the ticket names the commit (`set 12 commit=abc1234`).
