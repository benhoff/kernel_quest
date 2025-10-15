import importlib.util
from pathlib import Path
from unittest import mock

import pytest


MODULE_PATH = Path(__file__).resolve().parents[1] / "monster_client.py"
SPEC = importlib.util.spec_from_file_location("monster_client", MODULE_PATH)
monster_client = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(monster_client)


def test_send_line_writes_all_bytes_with_newline(monkeypatch):
    client = monster_client.MonsterClient()
    client.fd = 5
    chunks = []

    def fake_write(fd, data):
        if len(chunks) == 0:
            n = 2
        else:
            n = len(data)
        chunks.append(bytes(data[:n]))
        return n

    monkeypatch.setattr(monster_client.os, "write", mock.Mock(side_effect=fake_write))
    monkeypatch.setattr(monster_client.time, "sleep", lambda *_: None)

    client.send_line("ping")

    assert b"".join(chunks) == b"ping\n"


def test_send_line_requires_open_fd():
    client = monster_client.MonsterClient()
    with pytest.raises(RuntimeError):
        client.send_line("hello")


def test_print_msg_plain_mode_updates_last_state(monkeypatch):
    client = monster_client.MonsterClient(msg_prefix="[test] ")
    client._last_state = None
    messages = []

    monkeypatch.setattr(monster_client, "HAVE_PT", False)
    monkeypatch.setattr(monster_client, "print", lambda msg, flush=True: messages.append(msg), raising=False)

    client._print_msg("[STATE] idle")

    assert client._last_state == "[STATE] idle"
    assert messages == ["[test] [STATE] idle"]


def test_main_runs_startup_commands_and_skips_interactive(monkeypatch):
    created = {}

    class DummyClient:
        def __init__(self, *args, **kwargs):
            created["client"] = self
            self.open_called = False
            self.reader_started = False
            self.sent = []
            self.stopped = False
            self.closed = False
            self._msg_prefix = "[monster] "
            self._last_state = None

        def open(self):
            self.open_called = True

        def close(self):
            self.closed = True

        def start_reader(self):
            self.reader_started = True

        def stop_reader(self):
            self.stopped = True

        def send_line(self, line):
            self.sent.append(line)

    argv = [
        "monster_client.py",
        "--dev",
        "/tmp/monster",
        "--name",
        "Ada",
        "--cmd",
        "look",
        "--cmd",
        "feed",
        "--no-interactive",
    ]

    monkeypatch.setattr(monster_client, "MonsterClient", DummyClient)
    monkeypatch.setattr(monster_client.signal, "signal", lambda *_, **__: None)
    monkeypatch.setattr(monster_client.time, "sleep", lambda *_, **__: None)
    monkeypatch.setattr(monster_client, "HAVE_PT", False)
    monkeypatch.setattr(monster_client.sys, "argv", argv)

    exit_code = monster_client.main()

    client = created["client"]
    assert client.open_called
    assert client.reader_started
    assert client.sent == ["login Ada", "look", "feed"]
    assert client.stopped
    assert client.closed
    assert exit_code == 0


def test_lifecycle_message_updates_stage(monkeypatch):
    client = monster_client.MonsterClient(msg_prefix="[test] ")
    messages = []
    monkeypatch.setattr(monster_client, "HAVE_PT", False)
    monkeypatch.setattr(monster_client, "print", lambda msg, flush=True: messages.append(msg), raising=False)

    client._print_msg("[LIFECYCLE] Stage advanced to Growing!")
    assert client._stage == "Growing"

    client._print_msg("[LIFECYCLE] Current stage: Hatchling.")
    assert client._stage == "Hatchling"


def test_tip_commands_available_updates_toolbar(monkeypatch):
    client = monster_client.MonsterClient(msg_prefix="[test] ")
    client._stage = "Growing"
    monkeypatch.setattr(monster_client, "HAVE_PT", False)
    client._handle_tip_msg("[TIP] Commands available: look, go, state, grab <item>, feed <slot>.")

    assert "grab <item>" in client._available_commands
    assert "Stage: Growing" in client._toolbar_text
    assert "feed <slot>" in client._toolbar_text


def test_quest_message_tracks_last(monkeypatch):
    client = monster_client.MonsterClient(msg_prefix="[test] ")
    monkeypatch.setattr(monster_client, "HAVE_PT", False)
    client._print_msg("[QUEST] Goal: reach Growing (tick 120+, stability 40+).")

    assert client._last_quest == "[QUEST] Goal: reach Growing (tick 120+, stability 40+)."
