@echo off
setlocal
if not defined BAZEL_SH set "BAZEL_SH=bash.exe"
"%BAZEL_SH%" --noprofile --norc "%~dp0bazel_status.sh"
exit /b %errorlevel%
