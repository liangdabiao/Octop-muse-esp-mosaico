"""Review, compile and preview an Octop-produced procedural animation renderer.

Renders assets on the host; the device receives checked animation files only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from PIL import Image,ImageChops,ImageDraw

PROJECT=Path(__file__).resolve().parents[1]
MODES=('idle','listening','thinking','speaking','sleepy','error','happy','boot')
SIDE=128;FRAMES=40

def validate_source(source):
    includes=re.findall(r'^\s*#\s*include\s*([<"].*[>"])',source,re.M)
    allowed={'"muse_character.h"','<stdint.h>','<math.h>','<string.h>','<stdbool.h>'}
    if not includes or any(inc not in allowed for inc in includes):
        raise RuntimeError('Unexpected include in generated source')
    # This is a restricted drawing program, not arbitrary application code.
    if re.search(r'\b(?:system|popen|fopen|fwrite|printf|malloc|calloc|realloc|free|socket|exec|asm|__asm|LoadLibrary|GetProcAddress)\s*\(',source):
        raise RuntimeError('Unexpected side-effect API in generated renderer')
    if re.search(r'^\s*#\s*(?:pragma|undef|include_next|line)\b',source,re.M):
        raise RuntimeError('Unsupported directive in generated renderer')
    if 'muse_character_render' not in source:raise RuntimeError('Renderer function missing')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=PROJECT/'characters/muse-generated/muse_character.c')
    p.add_argument('--asan',action='store_true')
    args=p.parse_args();args.source=args.source.resolve()
    source=args.source.read_text(encoding='utf-8');validate_source(source)
    folder=args.source.parent;build=folder/('build-asan' if args.asan else 'build-preview')
    cmake=shutil.which('cmake') or str(PROJECT.parents[1]/'.codex-runs/idf-tools/tools/cmake/4.0.3/bin/cmake.exe')
    subprocess.run([cmake,'-S',str(PROJECT/'tools/character_renderer'),'-B',str(build),
        '-DCHARACTER_SOURCE='+str(args.source),'-DCHARACTER_ASAN='+('ON' if args.asan else 'OFF')],check=True,timeout=90)
    subprocess.run([cmake,'--build',str(build),'--config','Release'],check=True,timeout=90)
    exe=build/'Release/character_dump.exe' if os.name=='nt' else build/'character_dump'
    env=dict(os.environ)
    if args.asan and os.name=='nt':
        cache=(build/'CMakeCache.txt').read_text(encoding='utf-8')
        instance=re.search(r'^CMAKE_GENERATOR_INSTANCE:INTERNAL=(.+)$',cache,re.M)
        if instance:
            runtimes=list((Path(instance[1])/'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/clang_rt.asan_dynamic-x86_64.dll'))
            if runtimes:env['PATH']=str(sorted(runtimes)[-1].parent)+os.pathsep+env.get('PATH','')
    raw=folder/'frames.rgb';subprocess.run([str(exe),str(raw)],check=True,timeout=45,env=env)
    data=raw.read_bytes();frame_bytes=SIDE*SIDE*3
    if len(data)!=len(MODES)*FRAMES*frame_bytes:raise RuntimeError('Incomplete rendered frame data')
    assets=folder/'animations';assets.mkdir(exist_ok=True)
    contact=Image.new('RGB',(4*256,2*288),'black');draw=ImageDraw.Draw(contact)
    checks={}
    for mode_index,name in enumerate(MODES):
        frames=[Image.frombytes('RGB',(SIDE,SIDE),data[(mode_index*FRAMES+i)*frame_bytes:(mode_index*FRAMES+i+1)*frame_bytes]) for i in range(FRAMES)]
        changes=sum(bool(ImageChops.difference(frames[0],frame).getbbox()) for frame in frames[1:])
        if changes<FRAMES//3:raise RuntimeError(name+' animation is effectively static')
        if any(frame.getpixel((0,0))!=(0,0,0) for frame in frames):raise RuntimeError(name+' background is not black')
        strips=Image.new('RGB',(SIDE,SIDE*FRAMES))
        for i,frame in enumerate(frames):strips.paste(frame,(0,i*SIDE))
        palette=strips.quantize(colors=64,method=Image.Quantize.MEDIANCUT)
        quantized=[frame.quantize(palette=palette,dither=Image.Dither.NONE) for frame in frames]
        quantized[0].save(assets/(name+'.gif'),save_all=True,append_images=quantized[1:],duration=100,loop=0,optimize=False)
        frames[0].save(assets/(name+'.png'))
        x=mode_index%4*256;y=mode_index//4*288
        contact.paste(frames[10].resize((256,256),Image.Resampling.LANCZOS),(x,y))
        draw.text((x+12,y+264),name,fill='white')
        checks[name]=dict(frames=FRAMES,changed_from_first=changes,
            gif_sha256=hashlib.sha256((assets/(name+'.gif')).read_bytes()).hexdigest())
    contact.save(folder/'contact-sheet.png')
    evidence=dict(source_sha256=hashlib.sha256(args.source.read_bytes()).hexdigest(),
        source_check='restricted drawing APIs',memory_check='guarded output and fully written pixels',
        asan=args.asan,animations=checks)
    (folder/'preview-validation.json').write_text(json.dumps(evidence,indent=2),encoding='utf-8')
    raw.unlink()
    print('PASS: all eight animations have motion, complete bounded output and black backgrounds')
    print(folder/'contact-sheet.png')

if __name__=='__main__':main()
