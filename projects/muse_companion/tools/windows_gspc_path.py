"""Stage the local CMD adapter in the Windows tool cache without workspace spaces."""
import os
import shutil
from pathlib import Path
source=Path(__file__).parent
cache=Path(os.environ['LOCALAPPDATA'])/'esp-mosaico'/'build-adapters'/'muse_companion'
cache.mkdir(parents=True,exist_ok=True)
for name in ['gspc_windows_wrapper.py','gspc_windows_wrapper.cmd']:
    shutil.copy2(source/name,cache/name)
print((cache/'gspc_windows_wrapper.cmd').as_posix())
