"""Repository and script provenance, independent of individual experiments."""
from pathlib import Path
import subprocess
from .io import digest

ROOT = Path(__file__).resolve().parents[2]

def git_info() -> dict:
    try:
        def git(*args: str) -> str:
            return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True, stderr=subprocess.DEVNULL, timeout=10).strip()
        if Path(git("rev-parse", "--show-toplevel")).resolve() != ROOT:
            return {"commit": None, "branch": None, "dirty": None}
        return {"commit": git("rev-parse", "HEAD"), "branch": git("branch", "--show-current"),
                "dirty": bool(git("status", "--porcelain"))}
    except (OSError, subprocess.SubprocessError):
        return {"commit": None, "branch": None, "dirty": None}

def script_hashes(directory: Path) -> dict[str, str]:
    directory = directory.resolve()
    files = sorted(directory.glob("*.py")) + sorted((ROOT / "experiments/common").glob("*.py"))
    return {p.relative_to(ROOT).as_posix(): digest(p) for p in files}
