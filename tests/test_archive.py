"""Build and test the candidate source archive outside the source worktree."""
from pathlib import Path
import subprocess,sys,tarfile,tempfile
root=Path(__file__).resolve().parents[1]
def run(*args):subprocess.run([str(x) for x in args],check=True)
config=root/'build/archive-config'
run('cmake','-S',root,'-B',config,'-DSFC_TESTS=OFF','-DSFC_TOOLS=OFF')
with tempfile.TemporaryDirectory(prefix='sfc-archive-') as temp:
    temp=Path(temp)
    run(sys.executable,root/'tools/source_archive.py',config/'CPackSourceConfig.cmake',temp/'packages')
    archives=list((temp/'packages').glob('*.tar.gz'));assert len(archives)==1
    with tarfile.open(archives[0]) as tar:
        for member in tar.getmembers():
            path=Path(member.name)
            assert not path.is_absolute() and '..' not in path.parts
            assert '.git' not in path.parts and 'build' not in path.parts
            assert not any(part.startswith('._') for part in path.parts)
        tar.extractall(temp/'source',filter='data')
    sources=list((temp/'source').iterdir());assert len(sources)==1
    source=sources[0];build=temp/'build'
    run('cmake','-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DPython3_EXECUTABLE='+sys.executable)
    run('cmake','--build',build,'-j2')
    run('ctest','--test-dir',build,'--output-on-failure')
    run(build/'surface-compressor','--version')
print('Clean extracted candidate source archive passed')
