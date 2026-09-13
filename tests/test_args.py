#!/usr/bin/env python3
"""Argument checks that complete before GTK needs a display."""
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE = ROOT / "gtk-pipe"


def run(*args):
    return subprocess.run(
        [str(EXE), *args], capture_output=True, text=True, timeout=5
    )


help_result = run("--help")
assert help_result.returncode == 0, help_result.stderr
assert "--secure [--ini-file FILE]" in help_result.stdout

missing_secure = run("--ini-file", "/tmp/site-a.ini")
assert missing_secure.returncode != 0
assert "--ini-file requires --secure" in missing_secure.stderr

missing_value = run("--secure", "--ini-file")
assert missing_value.returncode != 0
assert "Unknown or incomplete option" in missing_value.stderr

duplicate = run(
    "--secure", "--ini-file", "/tmp/a.ini", "--ini-file", "/tmp/b.ini"
)
assert duplicate.returncode != 0
assert "Unknown or incomplete option" in duplicate.stderr

conflicting_aliases = run(
    "--secure", "--ini-file", "/tmp/a.ini", "--secure-config", "/tmp/b.ini"
)
assert conflicting_aliases.returncode != 0
assert "Unknown or incomplete option" in conflicting_aliases.stderr

print("arguments: --secure --ini-file help, requirement and duplicate checks passed")
