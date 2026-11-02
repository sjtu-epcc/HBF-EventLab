"""Run the supported nine-case matrix against one sealed incremental build."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from run_llama_integrated import ROOT, read, write, require
from integrated_profile import derive
from integrated_build_identity import add_build_arguments, verify_build
from setup_llama_baseline import sha

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--reference-matrix',type=Path,required=True)
    add_build_arguments(parser)
    a=parser.parse_args();out=a.output.resolve();reference=a.reference_matrix.resolve()
    identity=verify_build(a.build_dir,a.expected_backend)
    require(not out.exists(),'fresh matrix output required');out.mkdir(parents=True)
    write(out/'build-identity.json',identity)
    cases=[]
    for role,name in [('A','qwen3-0.6b'),('B','qwen3-1.7b'),('C','smollm2-135m')]:
        base=read(ROOT/'configs/integrated'/f'{name}.json')
        for prompt in (['short'] if role=='A' else ['short','long']):
            profile=derive(Path(base['model_path']),Path(base['conversion']),ubatch=64 if prompt=='long' else 256)
            profile_path=out/f'{name}-{prompt}.json';write(profile_path,profile)
            prompt_file=ROOT/'adapters/llama_cpp'/('prompt.txt' if prompt=='short' and role!='C' else f"prompt-integrated-{'smol' if role=='C' else 'qwen'}-{prompt}.txt")
            prompt_source=prompt_file
            prompt_file=out/f'{role}-{prompt}-prompt.txt'
            prompt_file.write_bytes(prompt_source.read_text(encoding='utf-8').encode('utf-8'))
            for tokens in ([32] if role=='A' else [32,64]):
                name_case=f'{role}-{prompt}-{tokens}';old=reference/name_case
                require(read(old/'profile.json')==profile,'reference profile mismatch '+name_case)
                require(sha(old/'prompt.txt')==sha(prompt_file),'reference prompt mismatch '+name_case)
                cases.append({'name':name_case,'profile':str(profile_path),'profile_sha256':sha(profile_path),
                    'prompt':str(prompt_file),'prompt_sha256':sha(prompt_file),'tokens':tokens,
                    'reference':str(old),'reference_modes':{mode:{'logits_sha256':sha(old/mode/'logits.f32'),
                        'summary_sha256':sha(old/mode/'summary.json')} for mode in ['native','bypass','modeled']}})
    write(out/'predeclared.json',{'cases':cases,'backend_commit':identity['backend']['commit'],
        'build_identity_sha256':sha(out/'build-identity.json')})
    results=[]
    for case in cases:
        folder=out/case['name'];old=Path(case['reference'])
        cmd=[sys.executable,str(ROOT/'tools/run_llama_integrated.py'),'--profile',case['profile'],
             '--prompt',case['prompt'],'--tokens',str(case['tokens']),'--output',str(folder),
             '--build-dir',str(a.build_dir),'--expected-backend',a.expected_backend]
        cmd += ['--gpu-device', a.gpu_device]
        if a.cuda_root is not None: cmd += ['--cuda-root', str(a.cuda_root)]
        print('Starting '+case['name'],flush=True)
        with (out/(case['name']+'.runner.log')).open('w',encoding='utf-8') as log:
            run=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
        result={'case':case['name'],'returncode':run.returncode,'comparisons':[]}
        results.append(result);write(out/'progress.json',{'status':'running','results':results})
        require(run.returncode==0,'matrix failed '+case['name']+'; logs retained')
        require(read(folder/'validation.json')['status']=='pass','validation missing')
        for mode in ['native','bypass','modeled']:
            frozen=case['reference_modes'][mode]
            require(sha(old/mode/'logits.f32')==frozen['logits_sha256'] and sha(old/mode/'summary.json')==frozen['summary_sha256'],'reference changed')
            require(sha(folder/mode/'logits.f32')==frozen['logits_sha256'],'logits changed '+case['name']+' '+mode)
            actual=read(folder/mode/'summary.json');previous=read(old/mode/'summary.json')
            require(actual['prompt_tokens']==previous['prompt_tokens'] and actual['generated_tokens']==previous['generated_tokens'],'tokens changed')
            result['comparisons'].append({'mode':mode,'complete_logits_byte_exact':True,'tokens_exact':True})
        result['status']='pass';write(out/'progress.json',{'status':'running','results':results})
        print('Passed '+case['name'],flush=True)
    require(verify_build(a.build_dir,a.expected_backend)==identity,'matrix build changed')
    write(out/'support-matrix.json',{'status':'pass','models':3,'architecture_families':2,
        'cases':results,'model_processes':27,'backend_commit':identity['backend']['commit'],
        'build_identity_sha256':sha(out/'build-identity.json')})

if __name__=='__main__':main()
