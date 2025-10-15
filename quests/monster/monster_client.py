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

def eprint(*a, **k):
    print(*a, file=sys.stderr, **k)

# Optional richer UX via prompt_toolkit
try:
    from prompt_toolkit import PromptSession, print_formatted_text
    from prompt_toolkit.patch_stdout import patch_stdout
    from prompt_toolkit.formatted_text import ANSI, HTML
    from html import escape
    HAVE_PT = True
except Exception:
    HAVE_PT = False
    PromptSession = None
    patch_stdout = None
    ANSI = None
    HTML = None
    print_formatted_text = None

class MonsterClient:
    def __init__(self, dev="/dev/monster", prompt="> ", enc="utf-8", read_chunk=4096,
                 use_colors=True, msg_prefix="[monster] "):
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
        lower = text.lower()
        if "[state]" in lower:
            self._last_state = text
        if HAVE_PT:
            if self._use_colors:
                # dim prefix using ANSI escape; body normal
                colorized = f"\x1b[2m{self._msg_prefix}\x1b[22m{text}"
            else:
                colorized = f"{self._msg_prefix}{text}"
            print_formatted_text(ANSI(colorized))
        else:
            print(f"{self._msg_prefix}{text}", flush=True)

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
    args = ap.parse_args()

    client = MonsterClient(dev=args.dev, enc=args.encoding, use_colors=not args.no_color)

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
        help_text = "Commands: login <name>, look, go <dir>, grab, analyze, feed, clean, rescue, clear, pet, debug, sing, inventory, state, say <msg>, reset, quit"
        # Escape '<' and '>' so HTML parser doesn't choke on <name>/<msg>
        toolbar = HTML(f'<ansigray>{escape(help_text)}</ansigray>') if HTML else help_text
        session = PromptSession(
            "> ",
            bottom_toolbar=toolbar,
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
