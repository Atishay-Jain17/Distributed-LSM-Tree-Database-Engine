@echo off
echo =====================================================================
echo  Distributed LSM-Tree Database Engine - Post-Reboot WSL & Build Setup
echo =====================================================================
echo.

wsl.exe --set-default-version 2

echo [1/3] Checking Ubuntu-24.04 registration...
wsl.exe -l -q | findstr /i "Ubuntu-24.04" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo Registering Ubuntu 24.04 in WSL...
    "C:\Program Files\WindowsApps\CanonicalGroupLimited.Ubuntu24.04LTS_2404.1.31.0_x64__79rhkp1fndgsc\ubuntu2404.exe" install --root
) else (
    echo Ubuntu-24.04 is already registered.
)

echo.
echo [2/3] Verifying WSL distribution status...
wsl.exe -l -v

echo.
echo [3/3] Running Linux build, POSIX SSTable and gRPC Raft cluster verification...
wsl.exe -d Ubuntu-24.04 -e bash -c "cd /mnt/d/PBL_V_Sem/week1/Distributed-LSM-Tree-Database-Engine && bash scripts/build_linux_all.sh"

echo.
echo =====================================================================
echo  Verification complete.
echo =====================================================================
pause
