"""Initialize dependency gitlinks for a clone or source ZIP."""
import json,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]
def git(*args):
    subprocess.run(['git','-C',str(root),*args],check=True)
if sys.version_info<(3,10):raise SystemExit('Python 3.10+ required')
if not (root/'.git').exists():git('init','-b','main')
pins=json.loads((root/'DEPENDENCIES.json').read_text())['submodules']
for name,sha in pins.items():
    result=subprocess.check_output(['git','-C',str(root),'ls-files','-s','--',name],text=True)
    if not result.strip():git('update-index','--add','--cacheinfo',f'160000,{sha},{name}')
git('submodule','update','--init','--recursive')
