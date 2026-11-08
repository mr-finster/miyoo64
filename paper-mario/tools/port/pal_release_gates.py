#!/usr/bin/env python3
"""Fatal PAL release gates, adapted from upstream miyoo.sh without rebuilding.

Reads only the supplied canonical ROM. No PERSONAL or unscanned release mode.
Source provenance uses the pinned decomp + committed patch and measured graphs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import tempfile
import zipfile
import zlib

PIN='ee321921df2debdaae4c9c00426b6ea84f26adb5'
PAL_SHA1='2111d39265a317414d359e35a7d971c4dfa5f9e1'

def git(root,*args):
    return subprocess.run(['git','-c','core.hooksPath=/dev/null','-C',str(root),*args],capture_output=True,check=True).stdout

def elf(data):
    assert data[:6]==b'\x7fELF\x01\x01' and struct.unpack_from('<HH',data,16)==(2,40)
    off=struct.unpack_from('<I',data,32)[0];ent,n,idx=struct.unpack_from('<HHH',data,46)
    rows=[struct.unpack_from('<10I',data,off+i*ent) for i in range(n)]
    s=rows[idx];strings=data[s[4]:s[4]+s[5]]
    return rows,{strings[x[0]:strings.find(b'\0',x[0])]:x for x in rows}

def pair(binary,assets,recipes):
    rows,secs=elf(binary);sec=secs[b'.assetdata'];assert sec[1]==8
    magic,version,addr,size,nrel,nsym,nnames,layout,assetcrc,dataoff=struct.unpack_from('<8s9I',assets)
    assert magic==b'PM64ASST' and version==1 and (addr,size)==(sec[3],sec[5])
    assert len(assets)==dataoff+size and dataoff%4096==0
    assert zlib.crc32(assets[44:44+nrel*4+nsym*16]+struct.pack('<I',size))&0xffffffff==layout
    payload=bytearray(assets[dataoff:]);assert zlib.crc32(payload)&0xffffffff==assetcrc
    expected=[]
    for off in struct.unpack_from('<%dI'%nrel,assets,44):
        value=struct.unpack_from('<I',payload,off)[0]
        expected.append((off,value-addr) if addr<=value<=addr+size else (off|0x80000000,value))
        payload[off:off+4]=b'\0'*4
    magic,rsize,crc,nrec,nlit,npat,nruns,nblocks,nrel2,maxblock,maxwords=struct.unpack_from('<8s10I',recipes)
    assert magic==b'PM64RCP1' and rsize==size and nrel2==nrel and nlit==0
    assert zlib.crc32(payload)&0xffffffff==crc
    cursor=(48+nrec*20+nlit+3)&~3;cursor+=npat*12+nruns*2;cursor=(cursor+3)&~3;cursor+=nblocks*12
    actual=[struct.unpack_from('<II',recipes,cursor+i*8) for i in range(nrel)];assert actual==expected
    cursor+=nrel*8;assert len(recipes)==cursor+16 and recipes[cursor:cursor+4]==b'BLID' and recipes[cursor+8:cursor+12]==b'STMP'
    cache,stamp=struct.unpack_from('<I',recipes,cursor+4)[0],struct.unpack_from('<I',recipes,cursor+12)[0]
    assert cache==zlib.crc32(struct.pack('<III',layout,addr,size)+b''.join(struct.pack('<II',*x) for x in actual)+struct.pack('<I',crc))&0xffffffff
    sy=secs[b'.symtab'];st=rows[sy[6]];strings=binary[st[4]:st[4]+st[5]];values={}
    for pos in range(sy[4],sy[4]+sy[5],16):
        no,v,length,info,other,idx=struct.unpack_from('<IIIBBH',binary,pos);name=strings[no:strings.find(b'\0',no)].decode()
        if name in ['port_build_stamp','__data_start','__assets_start','__start_dontsave','__stop_dontsave']:
            values[name]=v
            if name=='port_build_stamp':
                row=rows[idx];assert struct.unpack_from('<I',binary,row[4]+v-row[3])[0]==stamp
    assert len(values)==5 and stamp!=0
    return {'build_stamp':f'{stamp:08X}','cache_id':f'{cache:08X}','layout_id':f'{layout:08X}','recipe_crc':f'{crc:08X}','relocations':nrel,'literal_bytes':nlit},values

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['zip','rom','assets','binary','decomp','graph-root','output']:p.add_argument('--'+name,required=True,type=Path)
    a=p.parse_args(argv);root=Path(__file__).resolve().parents[2]
    assert a.rom.stat().st_size==67108864 and hashlib.sha1(a.rom.read_bytes()).hexdigest()==PAL_SHA1
    assert git(a.decomp,'rev-parse','HEAD').decode().strip()==PIN
    assert git(a.decomp,'diff','--binary',PIN,'--','.')==(root/'game/patches/decomp.patch').read_bytes()
    compiled=set()
    for name in ['miyoo.ninja','handheld.ninja']:
        text=(a.graph_root/name).read_text()
        compiled.update(re.findall(r'^build [^:]*: (?:cc_host|cc_game_port) /work/(\S+)',text,re.M))
    assert len(compiled)>=20
    for path in compiled:
        if path in ['build/game/pal/port_segtab.c','build/game/pal/port_effects.c','build/game/pal/miyoo/build_stamp.c','build/game/pal/handheld/build_stamp.c']:continue
        git(root,'ls-files','--error-unmatch',path)
    for pattern in ['game/port/*.c','game/port/*.h','kit/platform/*.c','kit/platform/*.h','bench/kit/*.c','bench/kit/*.h','miyoo/templates/*']:
        for f in root.glob(pattern):git(root,'ls-files','--error-unmatch',str(f.relative_to(root)))
    for f in ['miyoo/tools/miyoo_pack.py','miyoo/icon.png','miyoo/game-pal.json','tools/port/gen_build.py','tools/port/rom_segments.py','tools/port/pal_build.py','tools/port/pal_release_gates.py']:git(root,'ls-files','--error-unmatch',f)
    with tempfile.TemporaryDirectory(prefix='pal-release-gate-') as tmp:
        tmp=Path(tmp);prefix='App/PaperMarioPAL/';names=['pm64','recipes.bin','launch.sh','config.json','icon.png','full_resolution'];files={}
        with zipfile.ZipFile(a.zip) as z:
            assert z.testzip() is None and len(z.namelist())==6 and set(z.namelist())=={prefix+n for n in names}
            for row in z.infolist():
                name=Path(row.filename).name;mode=row.external_attr>>16
                assert stat.S_ISREG(mode) and stat.S_IMODE(mode)==(0o755 if name in ['pm64','launch.sh'] else 0o644)
                b=z.read(row);assert not re.search(rb'xmpmeta|Photoshop|/Users/|/home/|/root/|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|AKIA[0-9A-Z]{16}',b)
                assert not re.search(rb'PaperMarioPALTest|M143[469]|M14\.39',b)
                files[name]=tmp/name;files[name].write_bytes(b)
        assert files['pm64'].read_bytes()==a.binary.read_bytes()
        launch=files['launch.sh'].read_text();assert 'export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"' in launch
        assert 'export PM64_DATA="$DIR/data"' in launch and 'export HOME="$DIR"' in launch
        subprocess.run(['sh','-n',str(files['launch.sh'])],check=True)
        meta,values=pair(a.binary.read_bytes(),a.assets.read_bytes(),files['recipes.bin'].read_bytes())
        rel=subprocess.run(['readelf','-rW',str(a.binary)],capture_output=True,text=True,check=True).stdout
        for line in rel.splitlines():
            fields=line.split()
            if len(fields)>4 and fields[2]=='R_ARM_COPY':
                addr=int(fields[0],16)
                if values['__data_start']<=addr<values['__assets_start'] and not values['__start_dontsave']<=addr<values['__stop_dontsave']:
                    assert fields[4].startswith(('stdin','stdout','stderr'))
        subprocess.run(['python3','-B',str(root/'tools/assets/leak_scan.py'),'--rom',str(a.rom),'--assets',str(a.assets),'--budget-kb','48',*[str(files[n]) for n in names]],check=True)
    result={'status':'PASS','source_provenance':'pinned decomp + exact tracked patch; generated graphs and tracked host/port/packaging sources','compiled_host_port_sources':len(compiled),'pair':meta,'hardware_tested':False,'publication_authorized':False}
    a.output.write_text(json.dumps(result,indent=2)+'\n');print('PAL release gates PASS; hardware and publication approval remain separate.')
if __name__=='__main__':main()
