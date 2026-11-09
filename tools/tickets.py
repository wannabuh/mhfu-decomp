#!/usr/bin/env python3
"""The tickets: bugs, ideas and impressions, one folder each, with their evidence (docs/tickets.md).

    tools/tickets.py                            # what's open, highest priority first
    tools/tickets.py list [--all] [--status S] [--type T]
    tools/tickets.py check                      # ready for the user to check
    tools/tickets.py show 12
    tools/tickets.py new "Title" [--type bug] [--priority normal] [--status open] [--note "what was seen"]
                                                # priority high, normal or later (later is the status)
    tools/tickets.py note 12 "what was found"   # added to its findings, dated
    tools/tickets.py set 12 status=fix-ready commit=3c8326b check="fight something with a long tail"
    tools/tickets.py attach 12 0010 hits.log    # a capture by number is moved in; a file is copied in
    tools/tickets.py close 12                   # only at the user's word
    tools/tickets.py tui                        # all of it in the terminal, with vim's keys (tickets_tui.py)

They live in one place outside every checkout, not under version control: $MHFU_TICKETDIR, else
$XDG_DATA_HOME/mhfu-recomp/tickets, else ~/.local/share/mhfu-recomp/tickets. Commits name a ticket (#12), and the ticket
names the commit.
"""
import argparse
import datetime
import os
import re
import shutil
import sys

STATUSES = ["in-progress", "open", "fix-ready", "seems-original", "later", "closed"]
TYPES = ["bug", "graphics", "smoothness", "idea", "impression"]
PRIORITIES = ["high", "normal"]
FIELDS = ["status", "type", "priority", "filed", "commit", "check"]
NOT_OPEN = {"closed"}


def data_home():
    return os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")


def ticket_dir():
    return os.environ.get("MHFU_TICKETDIR") or os.path.join(data_home(), "mhfu-recomp", "tickets")


def capture_dir():
    return os.environ.get("MHFU_CAPTUREDIR") or os.path.join(data_home(), "mhfu-recomp", "captures")


def die(message):
    sys.exit(f"tickets: {message}")


class Ticket:
    def __init__(self, folder):
        self.folder = folder
        self.number = int(os.path.basename(folder).split("-")[0])
        self.path = os.path.join(folder, "ticket.md")
        with open(self.path) as f:
            lines = f.read().split("\n")
        self.title = lines[0].lstrip("# ").split(" ", 1)[1] if " " in lines[0].lstrip("# ") else ""
        self.fields, i = {}, 1
        while i < len(lines) and (not lines[i].strip() or re.match(r"[a-z]+:", lines[i])):
            m = re.match(r"([a-z]+):\s*(.*)", lines[i])
            if m:
                self.fields[m.group(1)] = m.group(2)
            i += 1
        self.body = "\n".join(lines[i:])

    def save(self):
        head = [f"# {self.number:04d} {self.title}", ""]
        head += [f"{k}: {self.fields.get(k, '')}".rstrip() for k in FIELDS]
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            f.write("\n".join(head) + "\n\n" + self.body.strip("\n") + "\n")
        os.replace(tmp, self.path)

    def add_to(self, section, text):
        mark = f"## {section}\n"
        if mark not in self.body:
            self.body = self.body.rstrip("\n") + f"\n\n{mark}\n"
        at = self.body.index(mark) + len(mark)
        end = self.body.find("\n## ", at)
        end = len(self.body) if end < 0 else end
        self.body = self.body[:end].rstrip("\n") + f"\n\n{text}\n" + self.body[end:]


def all_tickets():
    top = ticket_dir()
    if not os.path.isdir(top):
        return []
    found = []
    for name in sorted(os.listdir(top)):
        if re.match(r"\d+-", name) and os.path.exists(os.path.join(top, name, "ticket.md")):
            found.append(Ticket(os.path.join(top, name)))
    return found


def get(number):
    for t in all_tickets():
        if t.number == int(number):
            return t
    die(f"no ticket {number}")


def later(fields):
    """Priority "later" is the status: what's kept open and not worked on"""
    if fields.get("priority") == "later":
        fields["priority"], fields["status"] = "normal", "later"


def check_value(field, value):
    known = {"status": STATUSES, "type": TYPES, "priority": PRIORITIES}.get(field)
    if known and value not in known:
        die(f"{field} is one of {', '.join(known)}, not {value}")


def sort_key(t):
    status = t.fields.get("status", "open")
    return (STATUSES.index(status) if status in STATUSES else 0, t.fields.get("priority") != "high", t.number)


def line(t):
    f = t.fields
    evidence = sum(1 for n in os.listdir(t.folder) if n != "ticket.md")
    extra = f"  [{evidence} attached]" if evidence else ""
    return f"#{t.number:<4} {f.get('status', ''):<14} {f.get('priority', ''):<7} {f.get('type', ''):<11} {t.title}{extra}"


def cmd_list(a):
    shown = [t for t in all_tickets()
             if (a.all or a.status or t.fields.get("status") not in NOT_OPEN)
             and (not a.status or t.fields.get("status") == a.status)
             and (not a.type or t.fields.get("type") == a.type)]
    for t in sorted(shown, key=sort_key):
        print(line(t))
    if not shown:
        print("no tickets")


def cmd_check(a):
    ready = [t for t in all_tickets() if t.fields.get("status") == "fix-ready"]
    for t in sorted(ready, key=sort_key):
        print(f"#{t.number} {t.title}")
        if t.fields.get("commit"):
            print(f"    in: {t.fields['commit']}")
        print(f"    check: {t.fields.get('check') or '(nothing said: ask)'}")
    if not ready:
        print("nothing waiting for you")


def cmd_show(a):
    t = get(a.number)
    with open(t.path) as f:
        print(f.read(), end="")
    others = sorted(n for n in os.listdir(t.folder) if n != "ticket.md")
    print(f"\nfolder: {t.folder}" + (f"\nattached: {', '.join(others)}" if others else ""))


def cmd_new(a):
    if a.priority == "later":
        a.priority, a.status = "normal", "later"
    for field, value in (("type", a.type), ("priority", a.priority), ("status", a.status)):
        check_value(field, value)
    top = ticket_dir()
    os.makedirs(top, exist_ok=True)
    slug = re.sub(r"[^a-z0-9]+", "-", a.title.lower()).strip("-")[:48].rstrip("-")
    number = max([t.number for t in all_tickets()] + [0]) + 1
    while True:                         # (mkdir: two at once can't take the same number)
        folder = os.path.join(top, f"{number:04d}-{slug}")
        if not any(n.startswith(f"{number:04d}-") for n in os.listdir(top)):
            try:
                os.mkdir(folder)
                break
            except FileExistsError:
                pass
        number += 1
    with open(os.path.join(folder, "ticket.md"), "w") as f:
        f.write(f"# {number:04d} {a.title}\n\n")
    t = Ticket(folder)
    t.fields = {"status": a.status, "type": a.type, "priority": a.priority, "filed": str(datetime.date.today())}
    t.body = f"## What was seen\n\n{a.note or ''}\n\n## Findings\n"
    t.save()
    print(f"#{number} {folder}")


def cmd_note(a):
    t = get(a.number)
    t.add_to("Findings", f"{datetime.date.today()}: {a.text}")
    t.save()
    print(line(t))


def cmd_set(a):
    t = get(a.number)
    for pair in a.pairs:
        if "=" not in pair:
            die(f"{pair}: give field=value")
        field, value = pair.split("=", 1)
        if field == "title":
            t.title = value
            continue
        if field not in FIELDS:
            die(f"no field {field}: {', '.join(FIELDS)}")
        if field == "status" and value == "closed":
            die("closing is the user's: tools/tickets.py close")
        if not (field == "priority" and value == "later"):
            check_value(field, value)
        t.fields[field] = value
        later(t.fields)
    t.save()
    print(line(t))


def free_name(folder, name):
    if not os.path.exists(os.path.join(folder, name)):
        return name
    for i in range(2, 1000):
        stem, ext = os.path.splitext(name)
        if not os.path.exists(os.path.join(folder, f"{stem}-{i}{ext}")):
            return f"{stem}-{i}{ext}"
    die(f"too many called {name}")


def cmd_attach(a):
    t = get(a.number)
    for what in a.things:
        capture = os.path.join(capture_dir(), f"{int(what):04d}") if what.isdigit() else None
        file = what if os.path.exists(what) else os.path.join(capture_dir(), what)
        if capture and os.path.isdir(capture):
            name = free_name(t.folder, f"capture-{int(what):04d}")
            shutil.move(capture, os.path.join(t.folder, name))
            # (a file by its name: the game counts the next capture's number from it, capture.py follows it)
            with open(capture, "w") as f:
                f.write(os.path.join(t.folder, name) + "\n")
        elif capture and os.path.isfile(capture):
            with open(capture) as f:
                die(f"capture {what} is already in {f.read().strip()}")
        elif os.path.exists(file):
            name = free_name(t.folder, os.path.basename(file.rstrip("/")))
            target = os.path.join(t.folder, name)
            (shutil.copytree if os.path.isdir(file) else shutil.copy2)(file, target)
        else:
            die(f"nothing called {what} here or in {capture_dir()}")
        t.add_to("Evidence", f"- `{name}`")
        print(f"#{t.number}: {name}")
    t.save()


def cmd_close(a):
    t = get(a.number)
    t.fields["status"] = "closed"
    if a.note:
        t.add_to("Findings", f"{datetime.date.today()}: closed: {a.note}")
    t.save()
    print(line(t))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    s = p.add_subparsers(dest="command")
    c = s.add_parser("list")
    c.add_argument("--all", action="store_true")
    c.add_argument("--status")
    c.add_argument("--type")
    s.add_parser("check")
    s.add_parser("tui")
    s.add_parser("show").add_argument("number")
    c = s.add_parser("new")
    c.add_argument("title")
    c.add_argument("--type", default="bug")
    c.add_argument("--priority", default="normal")
    c.add_argument("--status", default="open")
    c.add_argument("--note")
    c = s.add_parser("note")
    c.add_argument("number")
    c.add_argument("text")
    c = s.add_parser("set")
    c.add_argument("number")
    c.add_argument("pairs", nargs="+")
    c = s.add_parser("attach")
    c.add_argument("number")
    c.add_argument("things", nargs="+")
    c = s.add_parser("close")
    c.add_argument("number")
    c.add_argument("--note")
    a = p.parse_args()
    if not a.command:
        a = p.parse_args(["list"])
    if a.command == "tui":
        import tickets_tui
        return tickets_tui.main()
    {"list": cmd_list, "check": cmd_check, "show": cmd_show, "new": cmd_new, "note": cmd_note, "set": cmd_set,
     "attach": cmd_attach, "close": cmd_close}[a.command](a)


if __name__ == "__main__":
    main()
