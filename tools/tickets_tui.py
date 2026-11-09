#!/usr/bin/env python3
"""The tickets in the terminal, with vim's keys (tools/tickets.py does the reading and writing).

    tools/tickets_tui.py            (or: tools/tickets.py tui)

The list:   j k  move     gg G  first, last     ^d ^u  half a page     Enter l  open
            /  search titles (Esc: all again)   a  closed ones too     f  only one status, in turn
            c  close (a note asked for)     o  open again     n  a note     p  priority high/normal
            L  later       v  the first screenshot       r  read again     ?  these keys     q  quit
The ticket: j k ^d ^u gg G  scroll    h q Esc  back    and c o n p L v as in the list
"""
import curses
import datetime
import os
import subprocess
import sys
import textwrap

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tickets as tk  # noqa: E402

FILTERS = [None, "fix-ready", "open", "in-progress", "later", "seems-original"]
COLORS = {"fix-ready": 1, "in-progress": 2, "later": 3, "closed": 4, "seems-original": 5}


class App:
    def __init__(self, scr):
        self.scr = scr
        self.all = False
        self.filter = 0
        self.search = ""
        self.sel = 0
        self.top = 0
        self.message = ""
        self.pending_g = False
        self.load()

    # the tickets shown
    def load(self):
        keep = self.shown[self.sel].number if getattr(self, "shown", None) and self.sel < len(self.shown) else None
        status = FILTERS[self.filter]
        found = [t for t in tk.all_tickets()
                 if (self.all or status or t.fields.get("status") not in tk.NOT_OPEN)
                 and (not status or t.fields.get("status") == status)
                 and self.search.lower() in t.title.lower()]
        self.shown = sorted(found, key=tk.sort_key)
        self.sel = next((i for i, t in enumerate(self.shown) if t.number == keep), min(self.sel, max(len(self.shown) - 1, 0)))

    def current(self):
        return self.shown[self.sel] if self.shown else None

    # drawing
    def put(self, y, x, text, attr=0):
        h, w = self.scr.getmaxyx()
        if 0 <= y < h and x < w:
            try:
                self.scr.addnstr(y, x, text, w - x - (1 if y == h - 1 else 0), attr)
            except curses.error:
                pass

    def status_attr(self, status):
        return curses.color_pair(COLORS.get(status, 0)) | (curses.A_DIM if status == "closed" else 0)

    def footer(self, keys):
        h, w = self.scr.getmaxyx()
        self.put(h - 1, 0, (self.message or keys).ljust(w), curses.A_REVERSE)
        self.message = ""

    def draw_list(self):
        self.scr.erase()
        h, w = self.scr.getmaxyx()
        what = [FILTERS[self.filter] or ("all" if self.all else "not closed")]
        if self.search:
            what.append(f"/{self.search}")
        self.put(0, 0, f" tickets: {len(self.shown)}, {', '.join(what)}", curses.A_BOLD)
        rows = h - 3
        if self.sel < self.top:
            self.top = self.sel
        if self.sel >= self.top + rows:
            self.top = self.sel - rows + 1
        for i, t in enumerate(self.shown[self.top:self.top + rows]):
            f = t.fields
            y = 2 + i
            mark = curses.A_REVERSE if self.top + i == self.sel else 0
            self.put(y, 0, " " * w, mark)
            self.put(y, 1, f"#{t.number:<4}", mark | curses.A_BOLD)
            self.put(y, 7, f"{f.get('status', ''):<14}", mark | self.status_attr(f.get("status")))
            hi = f.get("priority") == "high"
            self.put(y, 22, f"{'high' if hi else '':<5}", mark | (curses.color_pair(6) | curses.A_BOLD if hi else 0))
            self.put(y, 28, f"{f.get('type', ''):<11}", mark)
            self.put(y, 40, t.title, mark)
        if not self.shown:
            self.put(2, 1, "no tickets", curses.A_DIM)
        self.footer(" j/k move  Enter open  c close  o reopen  n note  p priority  / search  a all  f filter  ? keys  q quit")

    def lines_of(self, t, width):
        out = []
        for raw in open(t.path).read().split("\n"):
            indent = len(raw) - len(raw.lstrip())
            if not raw.strip():
                out.append("")
                continue
            out += textwrap.wrap(raw, width, subsequent_indent=" " * (indent + (2 if raw.lstrip().startswith("- ") else 0)),
                                 drop_whitespace=False) or [""]
        others = sorted(n for n in os.listdir(t.folder) if n != "ticket.md")
        out += ["", f"folder: {t.folder}"] + [f"  {n}" for n in others]
        return out

    def draw_ticket(self, t, scroll):
        self.scr.erase()
        h, w = self.scr.getmaxyx()
        lines = self.lines_of(t, w - 2)
        scroll = max(0, min(scroll, len(lines) - (h - 1)))
        for i, l in enumerate(lines[scroll:scroll + h - 1]):
            attr = curses.A_BOLD if l.startswith("#") else 0
            if l.startswith("status:"):
                attr = self.status_attr(l.split(":", 1)[1].strip()) | curses.A_BOLD
            self.put(i, 1, l, attr)
        self.footer(" j/k scroll  h back  c close  o reopen  n note  p priority  L later  v screenshot")
        return scroll, len(lines)

    # input at the bottom: Enter gives the text, Esc None
    def ask(self, prompt):
        curses.curs_set(1)
        text = ""
        while True:
            h, w = self.scr.getmaxyx()
            self.put(h - 1, 0, (prompt + text).ljust(w), curses.A_REVERSE)
            self.scr.move(h - 1, min(len(prompt + text), w - 2))
            k = self.scr.get_wch()
            if k in ("\n", "\r", curses.KEY_ENTER):
                break
            if k == "\x1b":
                text = None
                break
            if k in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                text = text[:-1]
            elif k == "\x15":            # ^u
                text = ""
            elif isinstance(k, str) and k.isprintable():
                text += k
        curses.curs_set(0)
        return text

    # what a key does to a ticket, in either view (True: done something)
    def act(self, k, t):
        if t is None:
            return False
        if k == "c":
            if t.fields.get("status") == "closed":
                self.message = f" #{t.number} is closed already"
                return True
            note = self.ask(f" close #{t.number}: a note (Enter: none, Esc: don't close) ")
            if note is not None:
                t.fields["status"] = "closed"
                if note.strip():
                    t.add_to("Findings", f"{datetime.date.today()}: closed: {note.strip()}")
                t.save()
                self.message = f" #{t.number} closed"
        elif k == "o":
            if t.fields.get("status") != "open":
                note = self.ask(f" open #{t.number} again: why (Enter: no note, Esc: don't) ")
                if note is not None:
                    t.fields["status"] = "open"
                    if note.strip():
                        t.add_to("Findings", f"{datetime.date.today()}: opened again: {note.strip()}")
                    t.save()
                    self.message = f" #{t.number} open"
        elif k == "n":
            note = self.ask(f" a note on #{t.number}: ")
            if note and note.strip():
                t.add_to("Findings", f"{datetime.date.today()}: {note.strip()}")
                t.save()
                self.message = f" noted on #{t.number}"
        elif k == "p":
            t.fields["priority"] = "normal" if t.fields.get("priority") == "high" else "high"
            t.save()
            self.message = f" #{t.number}: priority {t.fields['priority']}"
        elif k == "L":
            t.fields["status"] = "later"
            t.save()
            self.message = f" #{t.number}: later"
        elif k == "v":
            shots = sorted(os.path.join(r, n) for r, _, ns in os.walk(t.folder) for n in ns if n.endswith(".png"))
            if shots:
                subprocess.Popen(["xdg-open", shots[0]], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.message = f" opened {os.path.relpath(shots[0], t.folder)}"
            else:
                self.message = " no screenshot attached"
        else:
            return False
        return True

    # vim's moves: the new place from the old, a key, a page; None if not a move
    def move(self, k, at, last, page):
        if k == "g":
            if self.pending_g:
                self.pending_g = False
                return 0
            self.pending_g = True
            return at
        self.pending_g = False
        step = {"j": 1, curses.KEY_DOWN: 1, "k": -1, curses.KEY_UP: -1, "\x04": page // 2, "\x15": -(page // 2),
                "\x06": page, "\x02": -page, curses.KEY_NPAGE: page, curses.KEY_PPAGE: -page}
        if k == "G":
            return last
        if k in step:
            return max(0, min(last, at + step[k]))
        return None

    def ticket_view(self, t):
        scroll = 0
        while True:
            scroll, n = self.draw_ticket(t, scroll)
            k = self.scr.get_wch()
            h, _ = self.scr.getmaxyx()
            if k in ("h", "q", "\x1b", curses.KEY_LEFT):
                return
            if self.act(k, t):
                t = tk.Ticket(t.folder)
                continue
            m = self.move(k, scroll, max(n - (h - 1), 0), h - 1)
            if m is not None:
                scroll = m

    def run(self):
        curses.curs_set(0)
        curses.use_default_colors()
        for pair, color in ((1, curses.COLOR_GREEN), (2, curses.COLOR_YELLOW), (3, curses.COLOR_BLUE),
                            (4, -1), (5, curses.COLOR_MAGENTA), (6, curses.COLOR_RED)):
            curses.init_pair(pair, color, -1)
        while True:
            self.draw_list()
            k = self.scr.get_wch()
            h, _ = self.scr.getmaxyx()
            if k == "q":
                return
            if k in ("\n", "\r", "l", curses.KEY_ENTER, curses.KEY_RIGHT) and self.current():
                self.ticket_view(self.current())
                self.load()
            elif k == "/":
                s = self.ask(" /")
                self.search = s or ""
                self.sel = 0
                self.load()
            elif k == "\x1b":
                self.search = ""
                self.load()
            elif k == "a":
                self.all = not self.all
                self.load()
            elif k == "f":
                self.filter = (self.filter + 1) % len(FILTERS)
                self.sel = 0
                self.load()
            elif k == "r":
                self.load()
                self.message = " read again"
            elif k == "?":
                self.message = " gg/G first/last  ^d/^u half page  Enter/l open  h back  c close  o reopen  n note  p priority  L later  v screenshot"
            elif self.act(k, self.current()):
                self.load()
            else:
                m = self.move(k, self.sel, max(len(self.shown) - 1, 0), h - 3)
                if m is not None:
                    self.sel = m


def main():
    os.environ.setdefault("ESCDELAY", "25")      # (Esc at once, not after curses' second)
    curses.wrapper(lambda scr: App(scr).run())


if __name__ == "__main__":
    main()
