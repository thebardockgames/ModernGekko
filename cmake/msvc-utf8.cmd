@echo off
rem Compiler launcher for Ninja builds with a localized MSVC.
rem CMake writes the detected /showIncludes prefix into rules.ninja in the
rem console (OEM) code page, and MSVC prints it in that same code page. Do not
rem switch the code page here (e.g. chcp 65001): the accented prefix would no
rem longer match, Ninja would record no header dependencies, and header edits
rem would leave stale object layouts linked. Keep the installed compiler's
rem default language (English may be absent).
set VSLANG=
%*
exit /b %errorlevel%
