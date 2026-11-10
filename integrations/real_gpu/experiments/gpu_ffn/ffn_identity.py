"""Bind the FFN executable and current backend to a sealed llama dependency build."""
import argparse
import sys
from pathlib import Path
SOURCE = Path(__file__).resolve().parent
sys.path.insert(0, str(SOURCE.parent/'gpu_s'))
from build_identity import seal as _seal, verify as _verify
OPTIONS = dict(source=SOURCE, project='gpu_ffn', targets=('gpu_ffn', 'ffn_audit_tests'))
def seal(build):
    return _seal(build, **OPTIONS)
def verify(build):
    return _verify(build, **OPTIONS)
if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['seal', 'verify'])
    p.add_argument('--build', type=Path, required=True)
    a = p.parse_args()
    print((seal if a.action == 'seal' else verify)(a.build)['backend_source_sha256'])
