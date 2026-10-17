@echo off
setlocal
set "GSPC_WRAPPER_PYTHON=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe"
if not exist "%GSPC_WRAPPER_PYTHON%" set "GSPC_WRAPPER_PYTHON=python"
"%GSPC_WRAPPER_PYTHON%" "%~dp0gspc_windows_wrapper.py" %*
exit /b %ERRORLEVEL%
