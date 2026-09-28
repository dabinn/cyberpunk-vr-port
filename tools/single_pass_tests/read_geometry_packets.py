"""Copy bounded native render packets before the shared view scratch is reused."""
import argparse
import base64
import collections
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import struct
import sys
import time

root=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(root/'tools/swimming_tests'),str(root/'tools/roomscale_tests')]
from live_symbols import resolve
from live_read import Reader
from startup_guard import require_normal_window
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--pid',type=int,required=True);p.add_argument('--out',type=Path,required=True)
p.add_argument('--frames',type=int,choices=range(1,5),default=2)
p.add_argument('--resolve-inputs',action='store_true',help='Also copy the native tables referred to by temporary packet indices')
p.add_argument('--native-draws',action='store_true',help='Record actual scene draw bindings to validate preuploaded instance addresses')
args=p.parse_args()
symbols=resolve(args.pid,root/'build/bin/red4ext/plugins/CyberpunkVR_Stereo/Release/CyberpunkVR_Stereo.dll',
    ['CyberpunkVR_GeometryPacket*','CyberpunkVR_NativeStereo*','CyberpunkVR_StereoGpuProbeState','g_menuModeValue'])
reader=Reader(args.pid,r'C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe',0,0)
k=reader.k;k.WriteProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)];k.WriteProcessMemory.restype=w.BOOL
handle=k.OpenProcess(0x1028,False,args.pid);assert handle
prefix='CyberpunkVR_GeometryPacket';addr=lambda name:int(symbols[name],16)
read=lambda name:struct.unpack('<I',reader.read(addr(name),4))[0]
def write(name,value):
    data=c.c_uint32(value);done=c.c_size_t()
    if not k.WriteProcessMemory(handle,addr(name),c.byref(data),4,c.byref(done)) or done.value!=4:raise c.WinError(c.get_last_error())
armed=False;native_armed=False
try:
    assert read(prefix+'RecordBytes')==216,'Packet ABI mismatch'
    assert read('g_menuModeValue')==0,'Load gameplay first'
    require_normal_window(args.pid)
    assert read(prefix+'State')!=1 and read(prefix+'Request')==0,'Another packet recorder is active'
    if args.native_draws:
        assert read('CyberpunkVR_NativeStereoDrawBytes')==1096 and read('CyberpunkVR_NativeStereoAllDraws')==0,'Native draw ABI/mode differs'
        assert read('CyberpunkVR_NativeStereoProbeState')!=1 and read('CyberpunkVR_NativeStereoProbeRequest')==0,'Native recorder is active'
        assert read('CyberpunkVR_StereoGpuProbeState') not in (1,2),'GPU comparison is in flight'
    if args.resolve_inputs:
        assert read(prefix+'Resolve')==0 and read(prefix+'InputBytes')==256,'Input resolver is active or ABI differs'
        write(prefix+'Resolve',1);armed=True
    if args.native_draws:
        native_armed=True;write('CyberpunkVR_NativeStereoAllDraws',1);write('CyberpunkVR_NativeStereoProbeRequest',args.frames)
    write(prefix+'Request',args.frames);armed=True;deadline=time.monotonic()+8
    while time.monotonic()<deadline:
        state=read(prefix+'State')
        if read(prefix+'Request')==0 and state in (2,3):
            version=read(prefix+'Seq')
            if not(version&1):
                count=read(prefix+'Count');data_count=read(prefix+'DataCount');assert count<=256 and data_count<=262144
                records=reader.read(addr(prefix+'Records'),count*216) if count else b''
                packets=reader.read(addr(prefix+'Data'),data_count*16) if data_count else b''
                inputs=b'';input_blob=b'';input_count=0;input_failure=0;draws=b'';draw_count=0;native_full=False
                if args.resolve_inputs:
                    input_count=read(prefix+'InputCount');input_failure=read(prefix+'InputFailure');assert input_count<=65536
                    pointer=struct.unpack('<Q',reader.read(addr(prefix+'Inputs'),8))[0]
                    inputs=reader.read(pointer,input_count*256) if input_count else b''
                    blob_bytes=read(prefix+'InputBlobBytes');assert blob_bytes<=32*1024*1024
                    pointer=struct.unpack('<Q',reader.read(addr(prefix+'InputBlob'),8))[0]
                    input_blob=reader.read(pointer,blob_bytes) if blob_bytes else b''
                if args.native_draws:
                    native_state=read('CyberpunkVR_NativeStereoProbeState')
                    if native_state not in (2,3):time.sleep(.005);continue
                    native_seq=read('CyberpunkVR_NativeStereoProbeSeq')
                    if native_seq&1:continue
                    draw_count=read('CyberpunkVR_NativeStereoDrawCount');assert draw_count<=4096
                    draws=reader.read(addr('CyberpunkVR_NativeStereoDraws'),draw_count*1096) if draw_count else b''
                    if read('CyberpunkVR_NativeStereoProbeSeq')!=native_seq:continue
                    native_full=native_state==3
                if read(prefix+'Seq')==version and all(struct.unpack_from('<Q',records,i*216+16)[0] for i in range(count)):break
        time.sleep(.005)
    else:raise TimeoutError('Native packet capture did not finish within 8 seconds')
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.with_suffix('.records.bin').write_bytes(records);args.out.with_suffix('.packets.bin').write_bytes(packets)
    if args.resolve_inputs:
        args.out.with_suffix('.inputs.bin').write_bytes(inputs);args.out.with_suffix('.input-blob.bin').write_bytes(input_blob)
    if args.native_draws:args.out.with_suffix('.draws.bin').write_bytes(draws)
    rows=[]
    for i in range(count):
        data=records[i*216:(i+1)*216]
        row=dict(zip(('sequence','beginQpc','endQpc','renderer','arguments','context','view','storage','begin','end',
            'frame','node','side','thread','offset','count','status','metadataFlags'),struct.unpack_from('<10Q8I',data)))
        for name in ('renderer','arguments','context','view','storage','begin','end','node'):row[name]=hex(row[name])
        row['argumentBase64']=base64.b64encode(data[112:144]).decode()
        row['argumentPlane']=struct.unpack_from('<I',data,128)[0];row['modes']=list(data[136:139])
        row['nodeDefinition'],row['entry'],row['plane']=map(hex,struct.unpack_from('<3Q',data,144))
        if row['metadataFlags']&4:
            row['groupBase64']=base64.b64encode(data[168:216]).decode()
            row['groupFlags']=hex(struct.unpack_from('<I',data,176)[0])
            row['groupPlane']=struct.unpack_from('<I',data,184)[0];row['groupVariant']=data[188]
        rows.append(row)
    result={'pid':args.pid,'dll':symbols['sha256'],'full':state==3,'packetDataFile':args.out.with_suffix('.packets.bin').name,'records':rows,
        'inputDataFile':args.out.with_suffix('.inputs.bin').name if args.resolve_inputs else None,'inputFailure':input_failure,
        'inputBytes':256,'inputBlobFile':args.out.with_suffix('.input-blob.bin').name if args.resolve_inputs else None,
        'drawDataFile':args.out.with_suffix('.draws.bin').name if args.native_draws else None,'drawRecordBytes':1096,'drawsFull':native_full}
    args.out.write_text(json.dumps(result,indent=2))
    print(json.dumps({'pid':args.pid,'records':count,'packets':data_count,'full':state==3,
        'eyes':dict(collections.Counter(row['side'] for row in rows)),
        'statuses':dict(collections.Counter(row['status'] for row in rows)),
        'groupSnapshots':sum(bool(row['metadataFlags']&4) for row in rows),
        'resolvedInputs':input_count,'inputFailure':input_failure,'inputBlobBytes':len(input_blob),
        'nativeDraws':draw_count,'drawsFull':native_full,'out':str(args.out)},indent=2))
    assert count and state==2 and all(row['status'] in (0,1) for row in rows),'Incomplete packet snapshot'
    if args.resolve_inputs:assert input_failure==0 and input_count==data_count,'Incomplete native input snapshot'
    if args.native_draws:assert draw_count and not native_full,'Incomplete native draw snapshot'
finally:
    if armed:
        write(prefix+'Request',0);write(prefix+'State',0)
        if args.resolve_inputs:write(prefix+'Resolve',0)
    if native_armed:
        write('CyberpunkVR_NativeStereoProbeRequest',0);write('CyberpunkVR_NativeStereoProbeState',0);write('CyberpunkVR_NativeStereoAllDraws',0)
    k.CloseHandle(handle);reader.close()
