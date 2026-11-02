import hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
COMMIT="b3fa69955ea81c3b60e77e3c1e2224c4a826ffb9"
def sha(path):
    with Path(path).open("rb") as f:return hashlib.file_digest(f,"sha256").hexdigest()
