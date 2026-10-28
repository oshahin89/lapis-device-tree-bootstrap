if exist %~dp0images\anti_version.txt (for /f "delims==" %%a in (%~dp0images\anti_version.txt) do (set CURRENT_ANTI_VER=%%a))
if [%CURRENT_ANTI_VER%] EQU [] set CURRENT_ANTI_VER=0
for /f "tokens=2 delims=: " %%i in ('fastboot %* getvar anti 2^>^&1 ^| findstr /r /c:"anti:"') do (set version=%%i)
if [%version%] EQU [] set version=0
set anticheck="antirollback check pass"
if %version% GTR %CURRENT_ANTI_VER% set anticheck="Current device antirollback version is greater than this pakcage"
echo %anticheck% | findstr /r /c:"pass" || @echo "Antirollback check error" && exit /B 1
fastboot %* getvar product 2>&1 | findstr /r /c:"^product: *lapis" || @echo "error : Missmatching image and device" && pause && exit /B 1

::set CURRENT_ANTI_VER=1
::for /f "tokens=2 delims=: " %%i in ('fastboot %* getvar anti 2^>^&1 ^| findstr /r /#c:"anti:"') do (set version=%%i)
::if [%version%] EQU [] set version=0
::if %version% GTR %CURRENT_ANTI_VER% (
::	@echo "error : current device antirollback version is greater than this package"
::	exit /B 1
::)

if exist %~dp0images\flash_region_info.txt (for /f "delims==" %%a in (%~dp0images\flash_region_info.txt) do (set ROM_HWC=%%a))
fastboot %* getvar bypass_check_imgkey 2>&1 | findstr /r /c:"^bypass_check_imgkey: no"
if %errorlevel% equ 0 (
fastboot %* oem flash_check %ROM_HWC% || @echo "mix flash is not allowed error" && exit /B 1
)

fastboot %* getvar partition-type:opcust 2>&1 | findstr /r /c:"^partition-type:opcust: raw"
if %errorlevel% equ 0 (
fastboot %* erase opcust || @echo "Erase opcust error" && exit /B 1
)
fastboot %* getvar partition-type:opcust 2>&1 | findstr /r /c:"^partition-type:opcust: ext4"
if %errorlevel% equ 0 (
fastboot %* erase opcust || @echo "Erase opcust error" && exit /B 1
)
fastboot %* getvar partition-type:opconfig 2>&1 | findstr /r /c:"^partition-type:opconfig: raw"
if %errorlevel% equ 0 (
fastboot %* erase opconfig || @echo "Erase opconfig error" && exit /B 1
)
fastboot %* getvar partition-type:opconfig 2>&1 | findstr /r /c:"^partition-type:opconfig: ext4"
if %errorlevel% equ 0 (
fastboot %* erase opconfig || @echo "Erase opconfig error" && exit /B 1
)
fastboot %* erase boot || @echo "Erase boot error" && pause && exit /B 1

fastboot %* erase misc || @echo "Erase misc error" && pause && exit /B 1
fastboot %* erase metadata || @echo "Erase metadata error" && pause && exit /B 1
fastboot %* flash super %~dp0\images\super.img || @echo "Flash super error" && pause && exit /B 1
::fastboot %* flash cust %~dp0\images\cust.img || @echo "Flash cust error" && exit /B 1 error" && exit /B 1

fastboot %* flash preloader_ab %~dp0\images\preloader_lapis.bin || @echo "Flash preloader_a error" && pause && exit /B 1
::fastboot %* flash frp %~dp0\images\frp_zero.img || @echo "Flash frp error" && exit /B 1
fastboot %* flash vbmeta_ab %~dp0\images\vbmeta.img || @echo "Flash vbmeta_a error" && pause && exit /B 1
fastboot %* flash vbmeta_system_ab %~dp0\images\vbmeta_system.img || @echo "Flash vbmeta_system_a error" && pause && exit /B 1
fastboot %* flash vbmeta_vendor_ab %~dp0\images\vbmeta_vendor.img || @echo "Flash vbmeta_vendor_a error" && pause && exit /B 1
::fastboot %* flash md1img_ab %~dp0\images\md1img.img || @echo "Flash md1img_a error" && pause && exit /B 1
fastboot %* flash modem_ab %~dp0\images\modem.img || @echo "Flash modem error" && pause && exit /B 1
fastboot %* flash mcf_ota_ab %~dp0images\mcf_ota.img || @echo "Flash mcf_ota_ab error" && exit /B 1
fastboot %* flash spmfw_ab %~dp0\images\spmfw.img || @echo "Flash spmfw error" && pause && exit /B 1
fastboot %* flash pi_img_ab %~dp0\images\pi_img.img || @echo "Flash pi_a error" && pause && exit /B 1
fastboot %* flash dpm_ab %~dp0\images\dpm.img || @echo "Flash dpm_a error" && pause && exit /B 1
fastboot %* flash scp_ab %~dp0\images\scp.img || @echo "Flash scp_a error" && pause && exit /B 1
fastboot %* flash ccu_ab %~dp0images\ccu.img || @echo "Flash ccu_a error" && exit /B 1
fastboot %* flash vcp_ab %~dp0images\vcp.img || @echo "Flash vcp_a error" && exit /B 1
fastboot %* flash sspm_ab %~dp0\images\sspm.img || @echo "Flash sspm_a error" && pause && exit /B 1
fastboot %* flash mcupm_ab %~dp0\images\mcupm.img || @echo "Flash mcupm_a error" && pause && exit /B 1
fastboot %* flash gpueb_ab %~dp0images\gpueb.img || @echo "Flash gpueb_a error" && exit /B 1
fastboot %* flash apusys_ab %~dp0images\apusys.img || @echo "Flash apusys_a error" && exit /B 1
fastboot %* flash gz_ab %~dp0\images\gz.img || @echo "Flash gz_a error" && pause && exit /B 1
fastboot %* flash dtbo_ab %~dp0\images\dtbo.img || @echo "Flash dtbo_a error" && pause && exit /B 1
fastboot %* flash tee_ab %~dp0\images\tee.img || @echo "Flash tee_a error" && pause && exit /B 1
fastboot %* flash connsys_bt_ab %~dp0images\connsys_bt.img || @echo "Flash connsys_bt_a error" && exit /B 1
fastboot %* flash connsys_wifi_ab %~dp0images\connsys_wifi.img || @echo "Flash connsys_wifi_a error" && exit /B 1
fastboot %* flash connsys_gnss_ab %~dp0images\connsys_gnss.img || @echo "Flash connsys_gnss_a error" && exit /B 1
fastboot %* flash logo_ab %~dp0\images\logo.bin || @echo "Flash logo_a error" && pause && exit /B 1
::fastboot %* flash super %~dp0\images\super.img || @echo "Flash super error" && exit /B 1
fastboot %* flash userdata %~dp0\images\userdata.img || @echo "Flash userdata error" && pause && exit /B 1
fastboot %* flash boot_ab %~dp0\images\boot.img || @echo "Flash boot_a error" && pause && exit /B 1
fastboot %* flash vendor_boot_ab %~dp0\images\vendor_boot.img || @echo "Flash vendor_boot_a error" && pause && exit /B 1
fastboot %* flash init_boot_ab %~dp0images\init_boot.img || @echo "Flash init_boot_a error" && exit /B 1
::fastboot %* flash efuse %~dp0\images\efuse.img || @echo "Flash efuse error" && exit /B 1
fastboot %* flash rescue %~dp0\images\rescue.img || @echo "Flash rescue error" && pause && exit /B 1
fastboot %* flash countrycode_ab %~dp0\images\countrycode.img || @echo "Flash countrycode error" && pause && exit /B 1
::fastboot %* flash cust %~dp0\images\cust.img || @echo "Flash cust error" && exit /B 1 error" && exit /B 1
::fastboot %* flash oops %~dp0\images\oops.img || @echo "Flash oops error" && exit /B 1 error" && exit /B 1
fastboot %* flash lk_ab %~dp0\images\lk.img || @echo "Flash lk_a error" && pause && exit /B 1
fastboot %* flash misc %~dp0\images\misc.img || @echo "Flash misc error" && pause && exit /B 1
fastboot %* set_active a ||@echo "set_active  a error" && pause && exit /B 1
::fastboot %* erase misc || @echo "Erase misc error" && pause && exit /B 1
fastboot %* oem cdms
fastboot %* reboot || @echo "Reboot error" && pause && exit /B 1
