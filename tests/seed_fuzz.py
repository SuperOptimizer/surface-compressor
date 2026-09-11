from pathlib import Path
import struct,sys,hashlib
src=Path(__file__).resolve().parent/'fixtures';out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
def save(b): (out/hashlib.sha256(b).hexdigest()).write_bytes(b)
for p in src.glob('*.sfc'):
    b=p.read_bytes();save(b)
    for ch in range(struct.unpack_from('<I',b,8)[0]):
        ix,count=struct.unpack_from('<QQ',b,64+128*ch+96)
        for i in range(count):
            offset,length=struct.unpack_from('<QI',b,ix+32*i)
            save(b[offset:offset+length])
print('Seeded',len(list(out.iterdir())),'container/block inputs')
