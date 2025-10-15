import subprocess
import sys
from pathlib import Path

import pytest


def _resolve_paths():
    dev_path = Path("/dev/monster")
    client_path = Path(__file__).resolve().parents[1] / "monster_client.py"
    if not client_path.exists():
        pytest.skip("monster_client.py not found")
    if not dev_path.exists():
        pytest.skip("/dev/monster not present; load the module first")
    return dev_path, client_path


def run_client(*commands, timeout=5):
    dev_path, client_path = _resolve_paths()
    args = [
        sys.executable,
        str(client_path),
        "--dev",
        str(dev_path),
        "--no-interactive",
    ]
    for cmd in commands:
        args.extend(["--cmd", cmd])

    try:
        completed = subprocess.run(
            args,
            check=True,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.CalledProcessError as exc:
        stderr = exc.stderr or ""
        if "failed to open" in stderr or "No such file" in stderr:
            pytest.skip(f"monster device unavailable: {stderr.strip()}")
        raise
    return completed.stdout


def test_login_and_look_produces_room_description():
    output = run_client("login tester", "look", "quit")

    assert "[PROC] Helper thread tester spawned." in output
    assert "== /proc/nursery ==" in output


def test_state_command_reports_status():
    output = run_client("login status", "state", "quit")

    assert "[STATE] stability=" in output
    assert "Monster:" in output
