"""Batch paths for S-series runs, with defaults for archived entry points."""
import os
from pathlib import Path

from common import read

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = os.environ.get('GPU_S_BATCHES')
HISTORICAL = {
    's2-original': '20261031-s0-s2-002',
    's2': '20261101-s2-b2-v2-001',
    's3': '20261101-s3-b2-v2-001',
    's4': '20261101-s4-001',
    's5-zero': '20261101-s5-001',
    's5': '20261101-s5b-002',
    's6-pilot': '20261101-s6-pilot-001',
    's6': '20261101-s6-001',
    's6-qualified': '20261101-s6-minimum-acceptance-001',
    's7': '20261101-s7-001',
    's8-interrupted': '20261101-s8-001',
    's8': '20261101-s8-002',
}
BATCHES = read(Path(MANIFEST)) if MANIFEST else {}


def batch_path(stage):
    if not MANIFEST:
        return ROOT/'output/gpu-s'/HISTORICAL[stage]
    if stage not in BATCHES:
        raise ValueError(f'GPU_S_BATCHES has no path for {stage}: {MANIFEST}')
    return (Path(MANIFEST).resolve().parent/BATCHES[stage]).resolve()
