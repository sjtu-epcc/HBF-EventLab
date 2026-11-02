"""Verify historical standalone archives; create is disabled for the monorepo integration."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT=Path(__file__).resolve().parents[1]
MANIFEST='release-manifest.json'
EXCLUDED_ROOT_DIRS={'build','output','dist','models','.venv','.git','.pytest_cache'}

def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def sources(root):
    result=[]
    for p in sorted(root.rglob('*')):
        if not p.is_file():continue
        rel=p.relative_to(root)
        if rel.as_posix()==MANIFEST:continue
        if any(part in {'.git','__pycache__','.venv','.pytest_cache'} for part in rel.parts):continue
        if p.suffix=='.pyc':continue
        first=root/rel.parts[0]
        if first.is_dir() and (rel.parts[0] in EXCLUDED_ROOT_DIRS or rel.parts[0].startswith('build-')):continue
        if rel.parts[:2]==('configs','integrated') and p.suffix=='.json':continue
        if rel.as_posix()=='configs/machine.local.json':continue
        result.append(p)
    return result

def verify(archive):
    with zipfile.ZipFile(archive) as z:
        if z.testzip() is not None:raise RuntimeError('Archive CRC verification failed')
        manifest=json.loads(z.read(MANIFEST))
        expected={x['path'] for x in manifest['files']}|{MANIFEST}
        if len(z.namelist())!=len(expected) or set(z.namelist())!=expected:raise RuntimeError('Archive membership differs from manifest')
        for item in manifest['files']:
            p=Path(item['path'])
            if p.is_absolute() or '..' in p.parts:raise RuntimeError('Non-relative source member')
            content=z.read(item['path'])
            if len(content)!=item['bytes'] or hashlib.sha256(content).hexdigest()!=item['sha256']:raise RuntimeError('Changed archive member: '+item['path'])
        for name in ['third_party/llama.cpp-integrated/cmake/build-info.cmake',
                     'third_party/llama.cpp-integrated/common/build-info.h',
                     'third_party/HBF-EventLab/include/hbf_eventlab/runtime/simulation_session.hpp']:
            if name not in expected:raise RuntimeError('Required source missing: '+name)
    return manifest

def package(archive):
    raise RuntimeError(
        'Standalone real_gpu source packaging is no longer supported. '
        'Do not use the old standalone release package for this integration. '
        'From the HBF-EventLab repository root, run:\n'
        '  git archive --format=zip --output=<archive.zip> HEAD\n'
        'This exports committed files only; uncommitted integration changes are not included. '
        'The historical release-manifest.json is not rewritten. '
        'The verify action remains available for historical standalone archives.'
    )

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['create','verify'])
    parser.add_argument('--archive',type=Path,default=ROOT.parent/'RealGPUAcess_v0-source.zip')
    a=parser.parse_args()
    if a.action=='create':
        try:package(a.archive)
        except RuntimeError as error:parser.error(str(error))
    m=verify(a.archive)
    print(json.dumps({'status':'pass','files':len(m['files'])+1,'archive_sha256':sha(a.archive)},indent=2))
