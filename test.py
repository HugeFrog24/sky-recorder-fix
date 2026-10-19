import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('--compiler',default=os.environ.get('CXX') or shutil.which('g++') or shutil.which('clang++'))
parser.add_argument('--hoard',type=Path)
parser.add_argument('--update-signatures',action='store_true')
args=parser.parse_args()
if not args.compiler:
    parser.error('Set CXX, pass --compiler, or put g++ or clang++ on PATH')
root=Path(__file__).resolve().parent
out=root/'build/tests'
out.mkdir(parents=True,exist_ok=True)
source=(root/'src/fix.cpp').read_text()
(out/'adapter-under-test.inc').write_text(source[source.index('using Callback'):source.index('static int memory_protection(')])
compiler=[args.compiler,'-std=c++17','-O0','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-I',str(root/'src'),'-I',str(out)]
def compile_test(name,extra=(),flags=()):
    target=out/(name+('-quiet' if flags else '')+('.exe' if os.name=='nt' else ''))
    subprocess.run([*compiler,*flags,str(root/'tests'/(name+'.cpp')),*map(str,extra),'-o',str(target)],check=True)
    return target
locator=compile_test('test_locator',[root/'src/locator.cpp'])
expected=json.loads((root/'tests/fixtures/expected.json').read_text())
if args.update_signatures:
    roles=['Pixels','End','Begin','Complete','Register','Callback','PassPool']
    signatures={role:set() for role in roles}
    for item in expected:
        if 'fixture' not in item: continue
        result=subprocess.check_output([str(locator),str(root/'tests/fixtures'/item['fixture']),'--fingerprints',*map(str,item['functions'])],text=True)
        for role,line in zip(roles,result.splitlines()): signatures[role].add(tuple(map(int,line.split())))
    lines=['#pragma once','#include "locator.h"','','namespace sky {','inline constexpr Fingerprint signatures[]={']
    for role in roles:
        lines += ['    {0x%016xULL,0x%016xULL,%d,%s},'%(s[1],s[2],s[0],role) for s in sorted(signatures[role])]
    lines+=['};','}','']
    (root/'src/signatures.h').write_text('\n'.join(lines))
    print('Signatures regenerated from fixtures; rerun tests')
    raise SystemExit(0)
adapter=compile_test('test_adapter')
subprocess.run([str(adapter),str(out/'simulation.csv')],check=True)
quiet=compile_test('test_adapter',flags=['-DTEST_TRACE=0'])
subprocess.run([str(quiet)],check=True)
spec=importlib.util.spec_from_file_location('recorder_trace',root/'trace.py')
trace=importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)
rows,dropped=trace.read_trace(out/'simulation.csv')
analysis=trace.analyze(rows,dropped)
assert analysis['fix']['ownership_checks_passed'],analysis['fix']['errors']
assert analysis['fix']['saved_deliveries']==88
for bad in ['snapshot_failed','delivery_fallback','profile_rejected']:
    row=dict(rows[-1],event=bad)
    assert not trace.analyze([*rows,row],0)['fix']['ownership_checks_passed']
assert not trace.analyze(rows,1)['fix']['ownership_checks_passed']
assert not trace.analyze([r for r in rows if r['event']!='snapshot_wait'],0)['fix']['ownership_checks_passed']
assert not trace.analyze([r for r in rows if r['event']!='submit_begin'],0)['fix']['ownership_checks_passed']
timings=[]
for item in expected:
    paths=[]
    if 'fixture' in item: paths.append(root/'tests/fixtures'/item['fixture'])
    if args.hoard:
        path=args.hoard/item['file']
        with path.open('rb') as f: assert hashlib.file_digest(f,'sha256').hexdigest()==item['sha256'],path
        paths.append(path)
    for path in paths:
        run=subprocess.run([str(locator),str(path),'mutations'],text=True,capture_output=True,check=True)
        actual=json.loads(run.stdout.splitlines()[0])
        elapsed=actual.pop('microseconds')
        wanted={k:v for k,v in item.items() if k not in ('file','fixture','sha256')}
        assert actual==wanted,(path,actual,wanted)
        print(path.name,actual['status'],f'{elapsed/1000:.2f} ms')
        if args.hoard and path.parent==args.hoard: timings.append({'file':path.name,'microseconds':elapsed,**actual})
if args.hoard:
    actual_files={p.name for p in args.hoard.glob('libBootloader*.so')}
    assert actual_files=={x['file'] for x in expected},actual_files^{x['file'] for x in expected}
    (out/'hoard-results.json').write_text(json.dumps(timings,indent=2)+'\n')
print('Decoder fixtures, rejection, ASLR, ownership, restart and trace checks passed')
