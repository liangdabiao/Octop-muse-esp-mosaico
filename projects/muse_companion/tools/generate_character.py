"""Ask Octop through the device for an original character renderer by Mosaico RPC.

Device IO always uses mosaico.py; no serial port or token extraction is needed.
The returned C source is saved for review and never executed by this script.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

PROJECT=Path(__file__).resolve().parents[1]
WORKSPACE=PROJECT.parents[1]
SERVICE=0x7E10
DEFAULT_DESCRIPTION=(
    'A cute original ivory hooded plush companion, based on our existing character: '
    'rounded cream fabric hood and body, a small peach face recessed inside the hood, '
    'two tiny black dot eyes, a tiny friendly mouth, short rounded arms and feet. '
    'Warm ivory/beige highlights, restrained peach blush. Keep the identity and silhouette '
    'consistent in every state. Pure black background. Do not draw text or UI. '
    'Use soft smooth cartoon shapes and gentle shading, not pixel art.'
)

def rpc(method,payload=b''):
    before={p.name for p in (WORKSPACE/'.codex-runs/mosaico').glob('*-rpc')}
    env=dict(os.environ,PYTHONUTF8='1')
    cmd=[sys.executable,str(WORKSPACE/'mosaico.py'),'iris','rpc','--project',str(PROJECT),
         str(SERVICE),str(method),'--payload-hex',payload.hex()]
    result=subprocess.run(cmd,cwd=WORKSPACE,env=env,capture_output=True,text=True,encoding='utf-8',timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())
    dirs=[p for p in (WORKSPACE/'.codex-runs/mosaico').glob('*-rpc') if p.name not in before]
    if not dirs:raise RuntimeError('RPC evidence directory missing')
    log=max(dirs,key=lambda p:p.stat().st_mtime)/'raw.log'
    replies=[json.loads(line) for line in log.read_text(encoding='utf-8').splitlines()
             if line.startswith('{') and '"payload_base64"' in line]
    if not replies:raise RuntimeError('RPC response missing in evidence')
    return base64.b64decode(replies[-1]['payload_base64'])

def prompt(description):
    header=(PROJECT/'tools/character_renderer/muse_character.h').read_text(encoding='utf-8')
    return f'''Create an original animated character for my small companion screen.
You must design and implement its appearance and all its animations.
Character description: {description}

Return one complete portable C11 source file in a single ```c fence. No prose outside it.
At the top put a comment naming and describing the character. Under 14000 characters.
Implement exactly muse_character_render(const muse_character_pose_t *pose, uint32_t *pixels).
Include only "muse_character.h", <stdint.h>, <math.h>, <string.h>, <stdbool.h>.
This is a host renderer; float math in pixel loops is acceptable. It is NOT run on the ESP32.
No file/network/system calls, allocation, assembly, pragmas, or external dependencies.
Write every pixel as 0x00RRGGBB in the caller's 128x128 buffer. Background is pure black.
Use bounded ellipse, rounded rectangle, polygon or smooth analytic parts with anti-aliased edges,
gentle shading and expressive readable features. Keep the character centred, roughly x=64,
body width 50-65 pixels, height 88-100 pixels, feet near y=113; allow moving arms around it.
Check all pixel bounds. No alpha, no uninitialized pixels, no randomness between frames.
Compile cleanly in MSVC C11 and GCC: explicit casts when narrowing floats or integers.
Use portable functions, sinf/cosf/sqrtf/fabsf etc; no M_PI macro, use a named constant.

All looping states should have a smooth 4-second cycle (pose->t in [0,4)).
Idle: breathing, gentle bob, arm sway, and a blink during each cycle.
Listening: attentive expression, slight lean, hand cupping the ear and small sound waves.
Thinking: glance up, hand near chin, three softly moving thought dots.
Speaking: mouth movement, head bob and hand gestures; level drives mouth amplitude.
Sleepy: closed eyes, slow breathing and a tiny drifting sleep bubble without lettering.
Error: concerned expression, a brief head shake and a small warning mark.
Happy: hop, arms raised, smiling eyes and two floating peach/pink hearts.
Boot: a small greeting wave.
Do not return static images: each state must visibly animate while preserving identity.
Use the header below unchanged. Output only your C implementation.

```c
{header}
```
'''

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--description',default=DEFAULT_DESCRIPTION)
    parser.add_argument('--prompt-file',type=Path,help='A repair/edit prompt for Octop')
    parser.add_argument('--output',type=Path,default=PROJECT/'characters/muse-generated')
    parser.add_argument('--timeout',type=int,default=360)
    parser.add_argument('--resume',action='store_true',help='Read the current request after a host connection/download interruption')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    request=args.prompt_file.read_text(encoding='utf-8') if args.prompt_file else prompt(args.description)
    data=request.encode('utf-8')
    if len(data)>=32768:raise RuntimeError('Prompt exceeds 32767 bytes')
    if not args.resume:
        rpc(1)
        # Public Gateway RPC input is limited to 1024 bytes, including the offset.
        for at in range(0,len(data),1000):rpc(2,struct.pack('<I',at)+data[at:at+1000])
        rpc(3)
    start=time.monotonic();previous=None
    while time.monotonic()-start<args.timeout:
        state=json.loads(rpc(4))
        if state['state']=='input':raise RuntimeError('No submitted request to resume; run again without --resume')
        progress=(state['state'],state['reply_bytes'])
        if progress!=previous:
            print(f"octop {progress[0]} / received {progress[1]} bytes",flush=True);previous=progress
        if state['state'] in ('done','failed'):break
        time.sleep(3)
    else:
        rpc(6);raise RuntimeError('Generation timed out; cancellation requested')
    chunks=[]
    at=0
    while at<state['reply_bytes']:
        chunk=rpc(5,struct.pack('<I',at))
        if not chunk:raise RuntimeError('Incomplete generation reply')
        chunks.append(chunk);at+=len(chunk)
    reply=b''.join(chunks).decode('utf-8')
    if len(reply.encode('utf-8'))!=state['reply_bytes']:raise RuntimeError('Incomplete generation reply')
    (args.output/'muse-reply.md').write_text(reply,encoding='utf-8')
    (args.output/'prompt.txt').write_text(request,encoding='utf-8')
    if state['state']=='failed':raise RuntimeError(state['error'])
    blocks=re.findall(r'```(?:c|C)\s*\n(.*?)```',reply,re.S)
    if len(blocks)!=1:raise RuntimeError('Octop reply must contain exactly one complete C source fence')
    source=blocks[0].strip()+'\n'
    for name in ('muse_character.c','generation.json'):
        old=args.output/name
        if old.exists():(args.output/(name+'.prev')).write_bytes(old.read_bytes())
    (args.output/'muse_character.c').write_text(source,encoding='utf-8')
    evidence=dict(provider='Octop through the device link',description=args.description,
                  reply_bytes=len(reply.encode('utf-8')),source_sha256=hashlib.sha256((args.output/'muse_character.c').read_bytes()).hexdigest(),
                  generated_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
    (args.output/'generation.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2),encoding='utf-8')
    print(f"Octop source saved: {args.output/'muse_character.c'}",flush=True)

if __name__=='__main__':main()
