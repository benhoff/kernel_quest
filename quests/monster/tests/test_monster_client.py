import importlib.util
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1] / "monster_client.py"
SPEC = importlib.util.spec_from_file_location("monster_client", MODULE_PATH)
monster_client = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(monster_client)


class MonsterClientTests(unittest.TestCase):
    def test_send_line_writes_all_bytes_with_newline(self):
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

        with mock.patch.object(monster_client.os, "write", side_effect=fake_write):
            with mock.patch.object(monster_client.time, "sleep", return_value=None):
                client.send_line("ping")

        self.assertEqual(b"".join(chunks), b"ping\n")

    def test_send_line_requires_open_fd(self):
        client = monster_client.MonsterClient()
        with self.assertRaises(RuntimeError):
            client.send_line("hello")

    def test_print_msg_plain_mode_updates_last_state(self):
        client = monster_client.MonsterClient(msg_prefix="[test] ")
        client._last_state = None
        messages = []

        with mock.patch.object(monster_client, "HAVE_PT", False):
            with mock.patch.object(monster_client, "print", new=lambda msg, flush=True: messages.append(msg), create=True):
                client._print_msg("[STATE] idle")

        self.assertEqual(client._last_state, "[STATE] idle")
        self.assertEqual(messages, ["[test] [STATE] idle"])

    def test_main_runs_startup_commands_and_skips_interactive(self):
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
            "--dev", "/tmp/monster",
            "--name", "Ada",
            "--cmd", "look",
            "--cmd", "feed",
            "--no-interactive",
        ]

        with mock.patch.object(monster_client, "MonsterClient", DummyClient):
            with mock.patch.object(monster_client.signal, "signal", return_value=None):
                with mock.patch.object(monster_client.time, "sleep", return_value=None):
                    with mock.patch.object(monster_client, "HAVE_PT", False):
                        with mock.patch.object(monster_client.sys, "argv", argv):
                            exit_code = monster_client.main()

        client = created["client"]
        self.assertTrue(client.open_called)
        self.assertTrue(client.reader_started)
        self.assertEqual(client.sent, ["login Ada", "look", "feed"])
        self.assertTrue(client.stopped)
        self.assertTrue(client.closed)
        self.assertEqual(exit_code, 0)


if __name__ == "__main__":
    unittest.main()
