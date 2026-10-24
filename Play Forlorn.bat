@echo off
rem Starts Forlorn. Saves go to the "sandbox" folder next to this file; the log of the last run to forlorn-log.txt.
cd /d "%~dp0"
set HLE_QUIET=bridge,file,kvc,touch
start "Forlorn" /min cmd /c ""%~dp0ForlornHLE.exe" > "%~dp0forlorn-log.txt" 2>&1"
