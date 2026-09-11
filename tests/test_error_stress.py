"""Adversarial numeric roundtrips across independent strict/fast shared builds."""
import ctypes as C
import numpy as np
import pathlib,sys
libraries=[C.CDLL(str(pathlib.Path(p).resolve())) for p in sys.argv[1:]]
assert len(libraries)==2
for lib in libraries:
    lib.sfc_encode_xyz_block.argtypes=[C.c_void_p,C.c_size_t,C.c_size_t,C.c_void_p,C.c_double,C.POINTER(C.c_void_p),C.POINTER(C.c_size_t)]
    lib.sfc_decode_xyz_block.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t,C.c_size_t,C.c_void_p]
free=C.CDLL(None).free;free.argtypes=[C.c_void_p]
rng=np.random.default_rng(20260910);y,x=np.mgrid[:64,:64];cases=0;largest=0.
def ptr(a):return C.c_void_p(a.ctypes.data)
for origin in [0.,100000.,1000000.,4000000.]:
    for kind in range(8):
        bend=(np.abs(x-31.25)*7 if kind==0 else np.sin(x*.31)*np.cos(y*.17)*24)
        if kind==1:bend=rng.normal(0,30,(64,64))
        if kind==2:bend=np.where(x<32,-250.,250.)
        direction=rng.normal(size=3);direction/=np.linalg.norm(direction)
        a=np.stack([origin+x*.73,origin+y*.61,origin+1000+x*.17+y*.23],axis=-1)
        a+=bend[...,None]*direction
        a=a.astype('float32');mask=np.ones((64,64),dtype='uint8')
        if kind==3:mask=(rng.random((64,64))>.65).astype('uint8')
        if kind==4:mask=(x==y).astype('uint8')
        if kind==5:mask[:]=0;mask[31,30]=1
        if kind==6:mask[:]=0
        if kind==7:a[...,2]=np.float32(.001);mask=(x<47).astype('uint8')
        for error in [.25,1.]:
            for encoder in libraries:
                packet=C.c_void_p();size=C.c_size_t()
                assert encoder.sfc_encode_xyz_block(ptr(a),12,768,ptr(mask),error,C.byref(packet),C.byref(size))==0
                try:
                    for decoder in libraries:
                        out=np.empty_like(a);valid=np.empty_like(mask)
                        assert decoder.sfc_decode_xyz_block(packet,size.value,ptr(out),12,768,ptr(valid))==0
                        assert np.array_equal(valid,mask)
                        assert np.all(out[mask==0]==-1)
                        distances=np.linalg.norm(out[mask!=0].astype('float64')-a[mask!=0].astype('float64'),axis=-1)
                        maximum=float(distances.max(initial=0))
                        assert maximum<=error,(origin,kind,error,maximum)
                        assert np.all(out[...,2][mask!=0]>0)
                        largest=max(largest,maximum/error);cases+=1
                finally:free(packet)
print(f'{cases} cross-math XYZ stress decodes passed; largest fraction of budget {largest:.9g}')
