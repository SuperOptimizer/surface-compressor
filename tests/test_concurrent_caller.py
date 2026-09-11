"""External caller owns all threads; the codec remains entirely synchronous."""
import ctypes as C
import math
import pathlib
import tempfile
import sys
from concurrent.futures import ThreadPoolExecutor

lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
class Channel(C.Structure):
    _fields_=[('name',C.c_char*64),('width',C.c_uint64),('height',C.c_uint64),
              ('dtype',C.c_int),('flags',C.c_uint32),('components',C.c_uint32),
              ('component',C.c_uint32),('tolerance',C.c_double)]
lib.sfc_create.argtypes=[C.c_char_p,C.POINTER(Channel),C.c_uint32,C.c_void_p,C.c_size_t,C.POINTER(C.c_void_p)]
lib.sfc_write_block.argtypes=[C.c_void_p,C.c_void_p,C.c_size_t,C.c_size_t,C.c_void_p]
lib.sfc_finish.argtypes=[C.c_void_p]
lib.sfc_open_file.argtypes=[C.c_char_p,C.POINTER(C.c_void_p)]
lib.sfc_close.argtypes=[C.c_void_p]
lib.sfc_read_block.argtypes=[C.c_void_p,C.c_uint32,C.c_uint64,C.c_uint64,C.c_void_p,C.c_size_t,C.c_size_t,C.c_void_p]
lib.sfc_read_region.argtypes=[C.c_void_p,C.c_uint32,C.c_uint64,C.c_uint64,C.c_uint32,C.c_uint32,C.c_void_p,C.c_size_t,C.c_size_t,C.c_void_p]

def fixture(path):
    ch=(Channel*2)(Channel(b'x',2048,512,3,2,0,0,.1),Channel(b'y',2048,512,3,2,0,0,.1))
    writer=C.c_void_p()
    assert lib.sfc_create(str(path).encode(),ch,2,None,0,C.byref(writer))==0
    data=(C.c_float*4096)()
    for c in range(2):
        for b in range(256):
            for i in range(4096):
                data[i]=10000+c*100+b*.5+(i%64)*.1+(i//64)*.2+math.sin(i*.2)
            assert lib.sfc_write_block(writer,data,4,256,None)==0
    assert lib.sfc_finish(writer)==0

with tempfile.TemporaryDirectory(prefix='sfc-caller-') as tmp:
    path=pathlib.Path(tmp)/'data.sfc'
    fixture(path)
    reader=C.c_void_p()
    assert lib.sfc_open_file(str(path).encode(),C.byref(reader))==0
    def read(k):
        c=k%2;b=(k*97)%256
        out=(C.c_float*4096)();valid=(C.c_uint8*4096)()
        assert lib.sfc_read_block(reader,c,b%32,b//32,out,4,256,valid)==0
        assert all(valid)
        for i in range(0,4096,67):
            expected=C.c_float(10000+c*100+b*.5+(i%64)*.1+(i//64)*.2+math.sin(i*.2)).value
            assert abs(out[i]-expected)<=.1
        return bytes(out)
    reference=[read(k) for k in range(512)]
    # More callers than scratch slots, with table-cache eviction while borrowed.
    with ThreadPoolExecutor(max_workers=16) as executor:
        results=list(executor.map(read,list(range(512))*4))
    assert results==reference*4
    lib.sfc_close(reader)
print('external concurrent caller: shared reader, 2048 reads OK')
# Shared-reader XYZ scratch is leased independently of the shared-table cache.
lib.sfc_write_xyz_block.argtypes=lib.sfc_write_block.argtypes
lib.sfc_read_xyz.argtypes=lib.sfc_read_block.argtypes
with tempfile.TemporaryDirectory(prefix='sfc-xyz-caller-') as tmp:
    path=pathlib.Path(tmp)/'xyz.sfc'
    ch=(Channel*3)(*[Channel(k.encode(),512,128,3,6,3,a,.25) for a,k in enumerate('xyz')])
    writer=C.c_void_p();assert lib.sfc_create(str(path).encode(),ch,3,None,0,C.byref(writer))==0
    data=(C.c_float*(4096*3))();mask=(C.c_uint8*4096)()
    originals=[]
    for b in range(16):
        for i in range(4096):
            mask[i]=(i%64<51 and i//64>7)
            for k in range(3):data[3*i+k]=100000+k*300+b*.5+(i%64)*.13+(i//64)*.24+math.sin(i*.1)*.31*(k+1)
        originals.append(list(data))
        assert lib.sfc_write_xyz_block(writer,data,12,768,mask)==0
    assert lib.sfc_finish(writer)==0
    reader=C.c_void_p();assert lib.sfc_open_file(str(path).encode(),C.byref(reader))==0
    def read_xyz(b):
        out=(C.c_float*(4096*3))();valid=(C.c_uint8*4096)()
        assert lib.sfc_read_xyz(reader,0,b%8,b//8,out,12,768,valid)==0
        for i in range(0,4096,67):
            assert valid[i]==mask[i]
            if valid[i]:assert sum((out[3*i+k]-originals[b][3*i+k])**2 for k in range(3))<=.25**2
            else:assert all(out[3*i+k]==-1 for k in range(3))
        return bytes(out)
    expected=[read_xyz(b) for b in range(16)]
    with ThreadPoolExecutor(max_workers=16) as executor:
        assert list(executor.map(read_xyz,list(range(16))*32))==expected*32
    lib.sfc_close(reader)
print('external concurrent caller: joint XYZ shared reader, 512 reads OK')
# Cache sharing is exercised by caller-owned threads, without a threaded API.
lib.sfc_cache_create.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(C.c_void_p)]
lib.sfc_cache_destroy.argtypes=[C.c_void_p]
lib.sfc_cache_clear.argtypes=[C.c_void_p]
lib.sfc_cache_read_xyz_region.argtypes=lib.sfc_read_region.argtypes
reader=C.c_void_p();cache=C.c_void_p()
path=pathlib.Path(__file__).parent/'fixtures'/'v4-xyz.sfc'
assert lib.sfc_open_file(str(path).encode(),C.byref(reader))==0
assert lib.sfc_cache_create(reader,110000,C.byref(cache))==0
expected={}
for by in range(2):
    for bx in range(3):
        out=(C.c_float*(4096*3))();mask=(C.c_uint8*4096)()
        assert lib.sfc_read_xyz(reader,0,bx,by,out,12,768,mask)==0
        expected[bx,by]=(bytes(out)[:12],mask[0])
def cached(k):
    bx=k%3;by=(k//3)%2
    out=(C.c_float*3)();mask=(C.c_uint8*1)()
    if k%19==0:lib.sfc_cache_clear(cache)
    assert lib.sfc_cache_read_xyz_region(cache,0,bx*64,by*64,1,1,out,12,12,mask)==0
    assert (bytes(out),mask[0])==expected[bx,by]
with ThreadPoolExecutor(max_workers=16) as executor:
    list(executor.map(cached,range(1024)))
lib.sfc_cache_destroy(cache);lib.sfc_close(reader)
print('external concurrent caller: shared LRU cache, 1024 reads with eviction/clear OK')
# A virtual sparse source proves huge coordinates/index offsets need no giant
# file, full index, or full image allocation. Every tile aliases one raw packet.
import struct,zlib
raw=(pathlib.Path(__file__).parent/'fixtures'/'v1-exact.sfc').read_bytes()
header=bytearray(raw[:64]);descriptor=bytearray(raw[64:192])
index_offset=struct.unpack_from('<Q',descriptor,96)[0]
entry=bytearray(raw[index_offset:index_offset+32])
old_offset,n=struct.unpack_from('<QI',entry)
packet=raw[old_offset:old_offset+n]
edge=100_000_000;tiles=(edge+63)//64;data_offset=index_offset+tiles*tiles*32
struct.pack_into('<QQ',descriptor,64,edge,edge)
struct.pack_into('<Q',descriptor,104,tiles*tiles)
struct.pack_into('<QQ',header,32,data_offset,data_offset+n)
struct.pack_into('<I',header,48,zlib.crc32(header[:48]))
struct.pack_into('<Q',entry,0,data_offset)
requests=[]
CALLBACK=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.c_void_p,C.c_size_t)
@CALLBACK
def virtual_read(user,offset,dst,size):
    requests.append((offset,size))
    if offset==0 and size==64:payload=header
    elif offset==64 and size==128:payload=descriptor
    elif index_offset<=offset<data_offset and (offset-index_offset)%32==0 and size==32:payload=entry
    elif offset==data_offset and size==n:payload=packet
    else:return -1
    C.memmove(dst,bytes(payload),size);return 0
lib.sfc_open.argtypes=[CALLBACK,C.c_void_p,C.c_uint64,C.POINTER(C.c_void_p)]
lib.sfc_cache_read_region.argtypes=lib.sfc_read_region.argtypes
assert lib.sfc_open(virtual_read,None,data_offset+n,C.byref(reader))==0
assert lib.sfc_cache_create(reader,16384,C.byref(cache))==0
out=(C.c_uint16*1)();mask=(C.c_uint8*1)()
assert lib.sfc_cache_read_region(cache,0,edge-1,edge-1,1,1,out,2,2,mask)==0
assert mask[0]==1 and out[0]==(4095*7919)%65536
count=len(requests)
assert lib.sfc_cache_read_region(cache,0,edge-1,edge-1,1,1,out,2,2,mask)==0
assert len(requests)==count and count==4
assert sum(size for _,size in requests)<9000 and requests[2][0]>2**32
lib.sfc_cache_destroy(cache);lib.sfc_close(reader)
print('100M x 100M virtual image: far-corner access, <9KB I/O, 16KB cache budget OK')
