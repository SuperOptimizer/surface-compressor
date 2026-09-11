"""Integration tests: python test_cli.py /path/to/surface-compressor.
Requires numpy and tifffile (plus imagecodecs for compressed TIFF export).
"""
import pathlib
import subprocess
import sys
import tempfile
import numpy as np
import tifffile

exe = str(pathlib.Path(sys.argv[1]).resolve())
def run(*args, success=True):
    p = subprocess.run([exe, *map(str,args)],capture_output=True,text=True)
    assert (p.returncode == 0) == success, (args,p.stdout,p.stderr)
    return p

def image(path):
    with tifffile.TiffFile(path) as t:
        a=t.asarray()
        if t.pages[0].samplesperpixel>1 and t.pages[0].planarconfig==2:
            a=np.moveaxis(a,0,-1)
        return a

with tempfile.TemporaryDirectory(prefix='sfc-cli-') as tmp:
    root=pathlib.Path(tmp)
    src=root/'source';src.mkdir()
    y,x=np.mgrid[:70,:65]
    xyz=np.stack([10000+x*.27,20000+y*.31,3000+np.sin(x/15)+y*.09],-1).astype('float32')
    xyz[0,0]=0
    xyz[1,1,2]=.001
    for c,a in zip('xyz',np.moveaxis(xyz,-1,0)):
        tifffile.imwrite(src/f'{c}.tif',a,rowsperstrip=7)
    mask=np.full((140,130,2),255,dtype='uint8');mask[32,48,0]=0;mask[:,:,1]=17
    tifffile.imwrite(src/'mask.tif',mask,photometric='minisblack',planarconfig='contig',rowsperstrip=9)
    gen=((x+y*65)*11).astype('uint16');tifffile.imwrite(src/'generations.tif',gen,tile=(32,32))
    meta=b'{"format":"tifxyz","scale":[0.1,0.2],"unknown":{"keep":"verbatim"}}\n'
    (src/'meta.json').write_bytes(meta)
    encoded=root/'surface.sfc';out=root/'decoded'
    run('encode',src,encoded,'--error',.1);run('verify',encoded);run('decode',encoded,out)
    assert 'container 4' in run('--version').stdout
    run('encode',src,root/'invalid.sfc','--erorr',.25,success=False)
    run('encode',src,root/'invalid.sfc','--error',success=False)
    run('encode',src,root/'invalid.sfc','--error','',success=False)
    run('verify',encoded,'--unknown',success=False)
    run('decode',encoded,root/'extra','unexpected',success=False)
    assert not (root/'invalid.sfc').exists()

    got=np.stack([image(out/f'{c}.tif') for c in 'xyz'],-1)
    valid=np.ones((70,65),dtype=bool);valid[0,0]=False;valid[16,24]=False
    assert np.linalg.norm(xyz[valid].astype('float64')-got[valid],axis=-1).max()<=.1
    assert (got[~valid]==-1).all()
    assert (got[valid,2]>0).all()
    assert np.array_equal(image(out/'mask.tif'),mask)
    assert np.array_equal(image(out/'generations.tif'),gen)
    assert (out/'meta.json').read_bytes()==meta
    run('decode',encoded,out,success=False)
    # A pre-existing temporary file belongs to someone else. Exclusive-create
    # failure must preserve it, including the CLI's cleanup path.
    wrapper="import os,sys,pathlib; pathlib.Path(sys.argv[3]+'.tmp.'+str(os.getpid())).write_bytes(b'keep'); os.execv(sys.argv[1],[sys.argv[1],'encode',sys.argv[2],sys.argv[3]])"
    result=subprocess.run([sys.executable,'-c',wrapper,exe,str(src),str(root/'collision.sfc')],capture_output=True)
    assert result.returncode!=0
    collision=list(root.glob('collision.sfc.tmp.*'))
    assert len(collision)==1 and collision[0].read_bytes()==b'keep'

    # Generic 3-channel uint16 image, including partial blocks and planar input.
    rgb=np.stack([x*7,y*9,(x+y)*11],-1).astype('uint16')
    tifffile.imwrite(root/'image.tif',np.moveaxis(rgb,-1,0),photometric='rgb',planarconfig='separate',rowsperstrip=8)
    run('encode',root/'image.tif',root/'image.sfc','--error',2)
    run('decode',root/'image.sfc',root/'image-out')
    assert np.abs(image(root/'image-out'/'image.tif').astype('int32')-rgb).max()<=2
    with tifffile.TiffFile(root/'image-out'/'image.tif') as t:
        assert t.pages[0].photometric==2
    metrics=run('verify',encoded,'--reference',src).stdout
    assert all(key in metrics for key in ['quality xyz','mae=','rms=','psnr=','ssim_8x8='])
    rgba=np.concatenate([rgb,np.full((*rgb.shape[:2],1),65535,dtype='uint16')],axis=-1)
    tifffile.imwrite(root/'rgba.tif',rgba,photometric='rgb',extrasamples=['unassalpha'])
    run('encode',root/'rgba.tif',root/'rgba.sfc','--error',2)
    run('decode',root/'rgba.sfc',root/'rgba-out')
    assert np.abs(image(root/'rgba-out'/'image.tif').astype('int32')-rgba).max()<=2
    run('verify',root/'rgba.sfc','--reference',root/'rgba.tif')
    planes=np.stack([x+i*y for i in range(5)],-1).astype('uint16')
    tifffile.imwrite(root/'five.tif',planes,photometric='minisblack',planarconfig='contig')
    run('encode',root/'five.tif',root/'five.sfc','--error',2)
    run('decode',root/'five.sfc',root/'five-out')
    assert np.abs(image(root/'five-out'/'image.tif').astype('int32')-planes).max()<=2
    # Generic grayscale and floating multispectral images, including signed
    # values and exact preservation of exceptional float bit patterns.
    rng=np.random.default_rng(20260910)
    gray=np.clip(120+70*np.sin(x*.12)+rng.normal(0,7,x.shape),0,255).astype('uint8')
    tifffile.imwrite(root/'gray.tif',gray)
    run('encode',root/'gray.tif',root/'gray.sfc','--error',1)
    run('verify',root/'gray.sfc','--reference',root/'gray.tif')
    spectral=np.stack([np.sin(x*.1)*20,y*.7-30,(x-y)*.03,rng.normal(0,1,x.shape)],-1).astype('float32')
    spectral.view('uint32')[0,0,0]=0x7fc12345
    spectral[1,1,1]=np.inf;spectral[2,2,2]=-np.inf
    tifffile.imwrite(root/'spectral.tif',spectral,photometric='minisblack',planarconfig='contig')
    run('encode',root/'spectral.tif',root/'spectral.sfc','--error',.05)
    run('verify',root/'spectral.sfc','--reference',root/'spectral.tif')
    run('decode',root/'spectral.sfc',root/'spectral-out')
    got=image(root/'spectral-out'/'image.tif');finite=np.isfinite(spectral)
    assert np.max(np.abs(got[finite]-spectral[finite]))<=.05
    assert np.array_equal(got.view('uint32')[~finite],spectral.view('uint32')[~finite])
    # Corrupt a late payload: no partial output directory may become visible.
    data=bytearray(encoded.read_bytes());data[-1]^=32
    (root/'bad.sfc').write_bytes(data)
    run('decode',root/'bad.sfc',root/'bad-out',success=False)
    assert not (root/'bad-out').exists()
    assert not list(root.glob('bad-out.tmp.*'))
print('TIFF CLI integration passed')
