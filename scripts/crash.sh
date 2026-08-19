#!/usr/bin/env bash
# Show the newest UE crash context (error + symbolized callstack). Usage: scripts/crash.sh [N-th newest=1]
source "$(dirname "$0")/proton-env.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-1}"
D=$(ls -td "$ES2_SAVED"/Crashes/UECC-* 2>/dev/null | sed -n "${N}p")
[[ -z "$D" ]] && { echo "no crash dirs"; exit 1; }
F="$D/CrashContext.runtime-xml"
echo "== $D"
python3 - "$F" "$ROOT" <<'PY'
import sys,re,html,bisect,pickle,subprocess,os
t=open(sys.argv[1],errors='replace').read()
for tag in ['ErrorMessage','SecondsSinceStart','CrashType','IsEnsure','CommandLine']:
    m=re.search(rf'<{tag}>(.*?)</{tag}>',t,re.S); print(f'{tag}: {html.unescape(m.group(1)).strip() if m else "?"}')
m=re.search(r'<CallStack>(.*?)</CallStack>',t,re.S)
cs=html.unescape(m.group(1)) if m else ''
keys,names=pickle.load(open(os.path.join(sys.argv[2],'sdk/funcs.pkl'),'rb'))
dllsyms=None
def dllsym(off):
    global dllsyms
    if dllsyms is None:
        out=subprocess.run(['python3',os.path.join(sys.argv[2],'tools/symbolize_dll.py'),os.path.join(sys.argv[2],'mod/build/dwmapi.pdb'),hex(off)],capture_output=True,text=True).stdout.strip()
        return out
    return ''
print('CallStack:')
for line in cs.strip().split('\n'):
    line=line.strip()
    m=re.match(r'(\S+)\s+0x([0-9a-f]+) \+ ([0-9a-f]+)',line)
    if not m: print('  '+line); continue
    mod,base,off=m.group(1),int(m.group(2),16),int(m.group(3),16)
    if mod.startswith('ES2-Win64'):
        i=bisect.bisect_right(keys,off)-1
        print(f'  ES2 +{off:x}  {names[i][:150]} +{off-keys[i]:x}')
    elif mod=='dwmapi':
        print(f'  MOD +{off:x}  {dllsym(off)}')
    else: print(f'  {mod} +{off:x}')
PY
