"""Build a reproducible source tarball without macOS AppleDouble sidecars."""
import argparse,gzip,os,subprocess,tarfile
from pathlib import Path

def build(config,output):
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    subprocess.run(['cpack','--config',str(Path(config).resolve()),'-B',str(output.resolve())],check=True)
    archives=list(output.glob('*.tar.gz'))
    if len(archives)!=1:raise RuntimeError('expected exactly one source archive in output directory')
    path=archives[0];temporary=path.with_suffix('.normalizing');epoch=int(os.environ.get('SOURCE_DATE_EPOCH','0'))
    with tarfile.open(path) as source, temporary.open('wb') as stream:
        with gzip.GzipFile(filename='',mode='wb',fileobj=stream,mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed,mode='w',format=tarfile.PAX_FORMAT) as dest:
                seen=set()
                for member in sorted(source.getmembers(),key=lambda m:m.name):
                    parts=Path(member.name).parts
                    if any(p.startswith('._') for p in parts):continue
                    if Path(member.name).is_absolute() or '..' in parts or not (member.isfile() or member.isdir()):
                        raise RuntimeError('unexpected source archive member: '+member.name)
                    if member.name in seen:raise RuntimeError('duplicate member: '+member.name)
                    seen.add(member.name)
                    member.uid=member.gid=0;member.uname=member.gname='';member.mtime=epoch;member.pax_headers={}
                    member.mode=0o755 if member.isdir() else 0o644
                    dest.addfile(member,source.extractfile(member) if member.isfile() else None)
    temporary.replace(path);return path
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config');parser.add_argument('output');args=parser.parse_args()
    print(build(args.config,args.output))
