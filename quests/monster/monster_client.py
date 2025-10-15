#!/usr/bin/env python3
"""
monster_client.py — tiny interactive client for /dev/monster (prompt_toolkit + bottom toolbar)

- Clean separation of input (stable prompt) and device output (scrolling lines)
- Persistent commands reminder shown in a bottom toolbar while typing
- Uses prompt_toolkit when available; falls back to plain stdin otherwise
"""

import os
import sys
import argparse
import selectors
import signal
import threading
import time
import re

def eprint(*a, **k):
    print(*a, file=sys.stderr, **k)

# Optional richer UX via prompt_toolkit
try:
    from prompt_toolkit import PromptSession, print_formatted_text
    from prompt_toolkit.patch_stdout import patch_stdout
    from prompt_toolkit.formatted_text import ANSI, HTML
    from prompt_toolkit.completion import Completer, Completion
    from html import escape
    HAVE_PT = True
except Exception:
    HAVE_PT = False
    PromptSession = None
    patch_stdout = None
    ANSI = None
    HTML = None
    print_formatted_text = None
    Completer = object
    Completion = object

if HAVE_PT:
    class MonsterCompleter(Completer):
        def __init__(self, client: "MonsterClient") -> None:
            self.client = client

        def get_completions(self, document, complete_event):  # type: ignore[override]
            text = document.text_before_cursor
            if text.startswith("/"):
                options = ["/help", "/filter", "/filters", "/summary"]
                for option in options:
                    if option.startswith(text):
                        yield Completion(option, start_position=-len(text))
                return

            current_word = document.get_word_before_cursor(WORD=True) or ""
            for word in self.client.completion_words():
                if word.startswith(current_word):
                    yield Completion(word, start_position=-len(current_word))

class MonsterClient:
    VALID_CATEGORIES = {"state", "lifecycle", "tip", "quest", "event", "alert", "log"}

    def __init__(self, dev="/dev/monster", prompt="> ", enc="utf-8", read_chunk=4096,
                 use_colors=True, msg_prefix="[monster] ", theme="hud"):
        self.dev_path = dev
        self.fd = None
        self.sel = selectors.DefaultSelector()
        self.prompt = prompt
        self.enc = enc
        self.read_chunk = read_chunk
        self._stop = threading.Event()
        self._reader_thr = None
        self._partial = bytearray()
        self._session = None
        self._use_colors = use_colors
        self._msg_prefix = msg_prefix
        self._last_state = None
        self._stage = None
        self._available_commands = []
        self._toolbar_text = "Stage: ? | Commands: look, go, state"
        self._last_tip = None
        self._last_quest = None
        self._stats = {}
        self._quest_goal = None
        self._theme = theme
        self._hidden_categories = set()
        self._command_words = ["look", "go", "inventory", "state", "say", "quit", "login"]

    # ---- device I/O -----------------------------------------------------

    def open(self):
        # O_NONBLOCK lets us use selectors for smooth shutdowns
        self.fd = os.open(self.dev_path, os.O_RDWR | os.O_NONBLOCK)
        self.sel.register(self.fd, selectors.EVENT_READ)

    def close(self):
        try:
            if self.fd is not None:
                self.sel.unregister(self.fd)
                os.close(self.fd)
        finally:
            self.fd = None

    def send_line(self, line: str):
        if self.fd is None:
            raise RuntimeError("device not open")
        if not line.endswith("\n"):
            line = line + "\n"
        data = line.encode(self.enc, errors="replace")
        # Write in a loop to handle partial writes on char devices
        total = 0
        while total < len(data):
            try:
                n = os.write(self.fd, data[total:])
                if n <= 0:
                    raise OSError("short write")
                total += n
            except BlockingIOError:
                # brief backoff
                time.sleep(0.005)

    # ---- printing helpers ----------------------------------------------

    def set_session(self, session):
        """Attach a prompt_toolkit session so we can print safely while prompting."""
        self._session = session

    def _print_msg(self, text: str):
        """
        Thread-safe printing of device messages.
        - With prompt_toolkit available, use print_formatted_text(ANSI(...)) so the prompt redraws cleanly.
        - Otherwise, print to stdout with a simple prefix.
        """
        category = self._categorise(text)

        if category == "state":
            self._last_state = text
            self._handle_state_msg(text)
        elif category == "lifecycle":
            self._handle_lifecycle_msg(text)
        elif category == "tip":
            self._handle_tip_msg(text)
        elif category == "quest":
            self._last_quest = text
            self._handle_quest_msg(text)

        if category in self._hidden_categories:
            return
        if HAVE_PT:
            if self._use_colors:
                # dim prefix using ANSI escape; body normal
                colorized = f"\x1b[2m{self._msg_prefix}\x1b[22m{text}"
            else:
                colorized = f"{self._msg_prefix}{text}"
            print_formatted_text(ANSI(colorized))
        else:
            print(f"{self._msg_prefix}{text}", flush=True)

    def _handle_lifecycle_msg(self, text: str):
        match = re.search(r"Stage\s+advanced\s+to\s*([A-Za-z]+)", text, re.IGNORECASE)
        if not match:
            match = re.search(r"current\s+stage\s*:?[\s]*([A-Za-z]+)", text, re.IGNORECASE)
        if not match:
            match = re.search(r"Stage\s*:?[\s]*([A-Za-z]+)", text, re.IGNORECASE)
        if match:
            self._stage = match.group(1)
            self._refresh_toolbar()

    def _categorise(self, text: str) -> str:
        if text.startswith("[STATE]"):
            return "state"
        if text.startswith("[LIFECYCLE]"):
            return "lifecycle"
        if text.startswith("[TIP]"):
            return "tip"
        if text.startswith("[QUEST]"):
            return "quest"
        if text.startswith("[EVENT]"):
            return "event"
        if text.startswith("[ALERT]"):
            return "alert"
        return "log"

    def _handle_tip_msg(self, text: str):
        self._last_tip = text
        if "Commands available:" in text:
            _, tail = text.split("Commands available:", 1)
            commands = [segment.strip().strip('.') for segment in tail.split(',') if segment.strip()]
            self._available_commands = commands
            words = {cmd.split()[0] for cmd in commands if cmd}
            self._command_words = sorted(set(self._command_words) | words)
            self._refresh_toolbar()

    STATE_PATTERN = re.compile(
        r"stability=(?P<stability>-?\d+)\s+"
        r"hunger=(?P<hunger>-?\d+)\s+"
        r"mood=(?P<mood>-?\d+)\s+"
        r"trust=(?P<trust>-?\d+)\s+"
        r"tick=(?P<tick>\d+)\s+"
        r"junk=(?P<junk>-?\d+)\s+"
        r"(?:daemon_lost=(?P<daemon_lost>\w+))",
        re.IGNORECASE,
    )

    QUEST_PATTERN = re.compile(
        r"Goal:\s*reach\s+(?P<stage>[A-Za-z]+)\s*\(tick\s+(?P<tick>\d+)\+,\s*stability\s+(?P<stab>\d+)\+\)",
        re.IGNORECASE,
    )

    def _handle_state_msg(self, text: str):
        match = self.STATE_PATTERN.search(text)
        if match:
            parsed = match.groupdict()
            parsed_int = {
                key: (int(value) if key != "daemon_lost" else value.lower() in {"yes", "true", "1"})
                for key, value in parsed.items()
                if value is not None
            }
            self._stats = parsed_int
            self._refresh_toolbar()

    def _handle_quest_msg(self, text: str):
        match = self.QUEST_PATTERN.search(text)
        if match:
            data = match.groupdict()
            self._quest_goal = {
                "stage": data["stage"],
                "tick": int(data["tick"]),
                "stability": int(data["stab"]),
            }
        elif "retired" in text.lower():
            self._quest_goal = {
                "stage": "Retired",
                "tick": None,
                "stability": None,
            }
        self._refresh_toolbar()

    def _command_preview(self) -> str:
        if not self._available_commands:
            return "look, go, state"
        preview = ", ".join(self._available_commands[:7])
        if len(self._available_commands) > 7:
            preview += ", …"
        return preview

    def _quest_summary(self) -> str:
        if not self._quest_goal:
            return ""
        goal = self._quest_goal
        if goal["tick"] is None:
            return "Retired"
        current_tick = self._stats.get("tick")
        progress = f"{current_tick}/{goal['tick']}" if current_tick is not None else f"{goal['tick']}+"
        return f"{goal['stage']} (tick {progress}, stab {goal['stability']}+)"

    def _stats_summary(self) -> str:
        if not self._stats:
            return ""
        fields = []
        for label, key in (("Stab", "stability"), ("Hun", "hunger"), ("Mood", "mood"), ("Trust", "trust")):
            value = self._stats.get(key)
            if value is not None:
                fields.append(f"{label} {value}")
        return ", ".join(fields)

    def toggle_filter(self, category: str, hide: bool):
        if category not in self.VALID_CATEGORIES:
            print(f"[client] unknown category '{category}'")
            return
        if hide:
            self._hidden_categories.add(category)
        else:
            self._hidden_categories.discard(category)

    def filters_summary(self) -> str:
        if not self._hidden_categories:
            return "All categories visible"
        return "Hidden categories: " + ", ".join(sorted(self._hidden_categories))

    def summary_text(self) -> str:
        lines = []
        lines.append(f"Stage: {self._stage or '?'}")
        if self._quest_goal:
            quest = self._quest_summary() or self._quest_goal.get("stage", "")
            lines.append(f"Quest: {quest}")
        if self._stats:
            lines.append("Stats: " + self._stats_summary())
            lines.append(f"Tick: {self._stats.get('tick', '?')}, Junk: {self._stats.get('junk', '?')}, Daemon lost: {self._stats.get('daemon_lost', '?')}")
        if self._available_commands:
            lines.append("Commands: " + ", ".join(self._available_commands))
        if self._last_tip:
            lines.append(f"Last tip: {self._last_tip}")
        if self._last_quest:
            lines.append(f"Last quest: {self._last_quest}")
        return "\n".join(lines)

    def completion_words(self):
        words = set(self._command_words)
        words.update([cmd.split()[0] for cmd in self._available_commands if cmd])
        words.update(["/help", "/filter", "/filters", "/summary"])
        return sorted(words)

    def handle_local_command(self, cmd: str) -> bool:
        parts = cmd.split()
        if not parts:
            return True
        name = parts[0].lower()
        args = parts[1:]

        if name in {"/help", "/?"}:
            self._show_help()
        elif name == "/filter" and args:
            self._handle_filter_command(args)
        elif name == "/filters":
            print(self.filters_summary())
        elif name == "/summary":
            print(self.summary_text())
        else:
            print("[client] unknown local command. Try /help")
        return False

    def _show_help(self):
        print("[client] local commands:")
        print("  /help             show this help")
        print("  /filter [+|-]<cat>  hide or show category (tip,event,quest,alert,log,state,lifecycle)")
        print("  /filters          list current hidden categories")
        print("  /summary          display current stage, quest, and stats snapshot")

    def _handle_filter_command(self, args):
        for arg in args:
            hide = arg.startswith("-")
            show = arg.startswith("+")
            token = arg[1:] if (hide or show) else arg
            token = token.lower()
            if token == "all":
                if hide:
                    self._hidden_categories = set(self.VALID_CATEGORIES)
                elif show:
                    self._hidden_categories.clear()
                else:
                    print("[client] specify +all or -all")
                continue
            if hide:
                self.toggle_filter(token, True)
            elif show:
                self.toggle_filter(token, False)
            else:
                print("[client] prefix filters with + or - (e.g. /filter -tip)")
        print(self.filters_summary())

    def _refresh_toolbar(self):
        stage = self._stage or "?"
        if self._theme == "minimal":
            self._toolbar_text = "Stage: ? | Commands: look, go, state"
        else:
            stage = self._stage or "?"
            preview = self._command_preview()
            quest = self._quest_summary()
            stats = self._stats_summary()
            segments = [f"Stage: {stage}", f"Cmds: {preview}"]
            if quest:
                segments.append(f"Quest: {quest}")
            if stats:
                segments.append(stats)
            self._toolbar_text = " | ".join(segments)
        if self._session and HAVE_PT:
            try:
                self._session.app.invalidate()
            except Exception:
                pass

    # ---- reading thread --------------------------------------------------

    def _drain_device(self):
        """Read from device, print complete lines as they arrive."""
        try:
            while not self._stop.is_set():
                events = self.sel.select(timeout=0.25)
                if not events:
                    continue
                for key, _ in events:
                    try:
                        chunk = os.read(key.fd, self.read_chunk)
                        if not chunk:
                            # EOF from device
                            self._stop.set()
                            return
                        self._partial.extend(chunk)
                        # split on newlines; keep trailing partial
                        while True:
                            idx = self._partial.find(b"\n")
                            if idx < 0:
                                break
                            line = self._partial[:idx]
                            # strip CR if present
                            if line.endswith(b"\r"):
                                line = line[:-1]
                            del self._partial[:idx+1]
                            # print from device; avoid interfering with user typing
                            try:
                                decoded = line.decode(self.enc, errors="replace")
                            except Exception:
                                decoded = repr(line)
                            self._print_msg(decoded)
                    except BlockingIOError:
                        continue
                    except Exception as ex:
                        eprint(f"[monster] read error: {ex}")
                        self._stop.set()
                        return
        finally:
            # flush any remaining partial
            if self._partial:
                try:
                    decoded = self._partial.decode(self.enc, errors="replace")
                except Exception:
                    decoded = repr(bytes(self._partial))
                self._print_msg(decoded)
                self._partial.clear()

    def start_reader(self):
        self._reader_thr = threading.Thread(target=self._drain_device, daemon=True)
        self._reader_thr.start()

    def stop_reader(self):
        self._stop.set()
        if self._reader_thr:
            self._reader_thr.join(timeout=1.0)

# ---- CLI ---------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Interactive client for /dev/monster")
    ap.add_argument("-d", "--dev", default="/dev/monster", help="device path (default: /dev/monster)")
    ap.add_argument("--name", help="auto-login: send `login <name>` on connect")
    ap.add_argument("-c", "--cmd", action="append", default=[], help="command to send (can be repeated)")
    ap.add_argument("--no-interactive", action="store_true", help="do not drop into interactive shell after --cmd")
    ap.add_argument("--encoding", default="utf-8", help="text encoding (default: utf-8)")
    ap.add_argument("--no-echo-prompt", action="store_true", help="do not print local prompt (plain mode only)")
    ap.add_argument("--no-color", action="store_true", help="disable colored device messages")
    ap.add_argument("--status", action="store_true", help="print the latest [STATE] block on shutdown")
    ap.add_argument("--theme", choices=["hud", "minimal"], default="hud", help="UI theme (hud|minimal)")
    args = ap.parse_args()

    use_colors = not args.no_color and args.theme != "minimal"
    client = MonsterClient(dev=args.dev, enc=args.encoding, use_colors=use_colors, theme=args.theme)

    # Clean shutdown on Ctrl+C
    def handle_sigint(sig, frame):
        eprint("\n[monster] closing…")
        client.stop_reader()
        client.close()
        sys.exit(0)

    signal.signal(signal.SIGINT, handle_sigint)

    try:
        client.open()
    except Exception as ex:
        eprint(f"[monster] failed to open {args.dev}: {ex}")
        return 2

    client.start_reader()

    # Optional auto-login and scripted commands
    try:
        if args.name:
            client.send_line(f"login {args.name}")
        for c in args.cmd:
            client.send_line(c)
            # tiny delay so server can interleave responses nicely
            time.sleep(0.05)
    except Exception as ex:
        eprint(f"[monster] write error while sending startup commands: {ex}")

    if args.no_interactive:
        # Give reader a moment to drain the immediate responses
        time.sleep(0.2)
        client.stop_reader()
        client.close()
        return 0

    # Interactive loop
    if HAVE_PT:
        # Persistent commands reminder in the bottom toolbar
        help_text = "Stage: ? | Commands: look, go, state"
        client._toolbar_text = help_text

        def toolbar_provider():
            return HTML(f'<ansigray>{escape(client._toolbar_text)}</ansigray>') if HTML else client._toolbar_text

        completer = MonsterCompleter(client) if HAVE_PT else None
        session = PromptSession(
            "> ",
            bottom_toolbar=toolbar_provider,
            completer=completer,
        )
        client.set_session(session)
        try:
            # patch_stdout ensures background prints don't break the prompt line
            with patch_stdout():
                while True:
                    line = session.prompt()
                    if not line:
                        continue
                    cmd = line.strip()
                    if cmd.startswith('/'):
                        client.handle_local_command(cmd)
                        continue
                    if cmd.lower() in {"exit", "quit"}:
                        # still send "quit" to the device for courtesy
                        try:
                            client.send_line("quit")
                        except Exception:
                            pass
                        break
                    client.send_line(line)
        except (EOFError, KeyboardInterrupt):
            pass
    else:
        # Plain stdin fallback (no bottom toolbar capability here)
        try:
            while True:
                if not args.no_echo_prompt:
                    print("> ", end="", flush=True)
                line = sys.stdin.readline()
                if not line:  # EOF on stdin
                    break
                # allow quick exit
                if line.strip().lower() in {"exit", "quit"}:
                    try:
                        client.send_line("quit")
                    except Exception:
                        pass
                    break
                if line.lstrip().startswith('/'):
                    client.handle_local_command(line.strip())
                    continue
                client.send_line(line)
        except KeyboardInterrupt:
            pass

    client.stop_reader()
    client.close()
    if args.status and client._last_state:
        print(f"{client._msg_prefix}{client._last_state}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
