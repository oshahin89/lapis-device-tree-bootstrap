if [ -e $(dirname $0)/images/anti_version.txt ]; then
CURRENT_ANTI_VER=`cat $(dirname $0)/images/anti_version.txt`
fi
if [ -z "$CURRENT_ANTI_VER" ]; then CURRENT_ANTI_VER=0; fi
ver=`fastboot $* getvar anti 2>&1 | grep -oP "anti: \K[0-9]+"`
if [ -z "$ver" ]; then ver=0; fi
if [ $ver -gt $CURRENT_ANTI_VER ]; then echo "Current device antirollback version is greater than this pakcage"; exit 1; fi

fastboot $* getvar product 2>&1 | grep -E "^product: *lapis"

if [ $? -ne 0 ] ; then echo "error : Missmatching image and device"; exit 1; fi

#check flash version
if [ -e $(dirname $0)/images/flash_region_info.txt ]; then ROM_HWC=`cat $(dirname $0)/images/flash_region_info.txt`; fi
fastboot $* getvar bypass_check_imgkey 2>&1 | grep "^bypass_check_imgkey: no"
if [ $? -eq 0 ]; then
fastboot $* oem flash_check $ROM_HWC;
if [ $? -ne 0 ]; then echo "mix flash is not allowed error"; exit 1; fi
fi

#CURRENT_ANTI_VER=1
#version=`fastboot getvar anti 2>&1 | grep "anti:" | awk -F ": " '{print $2}'`
#if [  "${version}"x == ""x ] ; then version=0 ; fi
#if [ ${version} -gt ${CURRENT_ANTI_VER} ] ; then  echo "error : current device antirollback #version is greater than this package" ; exit 1 ; fi

fastboot $* getvar partition-type:opcust 2>&1 | grep "^partition-type:opcust: raw"
if [ $? -eq 0 ]; then
    fastboot $* erase opcust
    if [ $? -ne 0 ] ; then echo "Erase opcust error"; exit 1; fi
else
    echo "No partition opcust found, skip erase opcust"
fi
fastboot $* getvar partition-type:opcust 2>&1 | grep "^partition-type:opcust: ext4"
if [ $? -eq 0 ]; then
    fastboot $* erase opcust
    if [ $? -ne 0 ] ; then echo "Erase opcust error"; exit 1; fi
else
    echo "No partition opcust found, skip erase opcust"
fi
fastboot $* getvar partition-type:opconfig 2>&1 | grep "^partition-type:opconfig: raw"
if [ $? -eq 0 ]; then
    fastboot $* erase opconfig
    if [ $? -ne 0 ] ; then echo "Erase opconfig error"; exit 1; fi
else
    echo "No partition opconfig found, skip erase opconfig"
fi
fastboot $* getvar partition-type:opconfig 2>&1 | grep "^partition-type:opconfig: ext4"
if [ $? -eq 0 ]; then
    fastboot $* erase opconfig
    if [ $? -ne 0 ] ; then echo "Erase opconfig error"; exit 1; fi
else
    echo "No partition opconfig found, skip erase opconfig"
fi
fastboot $* erase misc
fastboot $* erase boot
if [ $? -ne 0 ] ; then echo "Erase boot error"; exit 1; fi
fastboot $* erase metadata
if [ $? -ne 0 ] ; then echo "Erase metadata error"; exit 1; fi
fastboot $* flash crclist `dirname $0`/images/crclist.txt
if [ $? -ne 0 ] ; then echo "Flash crclist error"; exit 1; fi
fastboot $* flash sparsecrclist `dirname $0`/images/sparsecrclist.txt
if [ $? -ne 0 ] ; then echo "Flash sparsecrclist error"; exit 1; fi
fastboot $* flash preloader_ab `dirname $0`/images/preloader_lapis.bin
if [ $? -ne 0 ] ; then echo "Flash preloader_a error"; exit 1; fi
#fastboot $* flash frp `dirname $0`/images/frp_zero.img
#if [ $? -ne 0 ] ; then echo "Flash frp error"; exit 1; fi
fastboot $* flash vbmeta_ab `dirname $0`/images/vbmeta.img
if [ $? -ne 0 ] ; then echo "Flash vbmeta_a error"; exit 1; fi
fastboot $* flash vbmeta_system_ab `dirname $0`/images/vbmeta_system.img
if [ $? -ne 0 ] ; then echo "Flash vbmeta_system_a error"; exit 1; fi
fastboot $* flash vbmeta_vendor_ab `dirname $0`/images/vbmeta_vendor.img
if [ $? -ne 0 ] ; then echo "Flash vbmeta_vendor_a error"; exit 1; fi
#fastboot $* flash md1img_ab `dirname $0`/images/md1img.img
#if [ $? -ne 0 ] ; then echo "Flash md1img_a error"; exit 1; fi
fastboot $* flash modem_ab `dirname $0`/images/modem.img
if [ $? -ne 0 ] ; then echo "Flash modem error"; exit 1; fi
fastboot $* flash mcf_ota_ab `dirname $0`/images/mcf_ota.img
if [ $? -ne 0 ] ; then echo "Flash mcf_ota_ab error"; exit 1; fi
fastboot $* flash spmfw_ab `dirname $0`/images/spmfw.img
if [ $? -ne 0 ] ; then echo "Flash spmfw_a error"; exit 1; fi
fastboot $* flash pi_img_ab `dirname $0`/images/pi_img.img
if [ $? -ne 0 ] ; then echo "Flash pi_a error"; exit 1; fi
fastboot $* flash dpm_ab `dirname $0`/images/dpm.img
if [ $? -ne 0 ] ; then echo "Flash dpm_a error"; exit 1; fi
fastboot $* flash scp_ab `dirname $0`/images/scp.img
if [ $? -ne 0 ] ; then echo "Flash scp_a error"; exit 1; fi
fastboot $* flash ccu_ab `dirname $0`/images/ccu.img
if [ $? -ne 0 ] ; then echo "flash ccu_a error"; exit 1; fi
fastboot $* flash vcp_ab `dirname $0`/images/vcp.img
if [ $? -ne 0 ] ; then echo "flash vcp_a error"; exit 1; fi
fastboot $* flash sspm_ab `dirname $0`/images/sspm.img
if [ $? -ne 0 ] ; then echo "Flash sspm_a error"; exit 1; fi
fastboot $* flash mcupm_ab `dirname $0`/images/mcupm.img
if [ $? -ne 0 ] ; then echo "Flash mcupm_a error"; exit 1; fi
fastboot $* flash gpueb_ab `dirname $0`/images/gpueb.img
if [ $? -ne 0 ] ; then echo "flash gpueb_a error"; exit 1; fi
fastboot $* flash apusys_ab `dirname $0`/images/apusys.img
if [ $? -ne 0 ] ; then echo "flash apusys_a error"; exit 1; fi
fastboot $* flash gz_ab `dirname $0`/images/gz.img
if [ $? -ne 0 ] ; then echo "Flash gz_a error"; exit 1; fi
fastboot $* flash lk_ab `dirname $0`/images/lk.img
if [ $? -ne 0 ] ; then echo "Flash lk_a error"; exit 1; fi
fastboot $* flash dtbo_ab `dirname $0`/images/dtbo.img
if [ $? -ne 0 ] ; then echo "Flash dtbo_a error"; exit 1; fi
fastboot $* flash tee_ab `dirname $0`/images/tee.img
if [ $? -ne 0 ] ; then echo "Flash tee_a error"; exit 1; fi
fastboot $* flash connsys_bt_ab `dirname $0`/images/connsys_bt.img
if [ $? -ne 0 ] ; then echo "flash connsys_bt_a error"; exit 1; fi
fastboot $* flash connsys_wifi_ab `dirname $0`/images/connsys_wifi.img
if [ $? -ne 0 ] ; then echo "flash connsys_wifi_a error"; exit 1; fi
fastboot $* flash connsys_gnss_ab `dirname $0`/images/connsys_gnss.img
if [ $? -ne 0 ] ; then echo "flash connsys_gnss_a error"; exit 1; fi
fastboot $* flash logo_ab `dirname $0`/images/logo.bin
if [ $? -ne 0 ] ; then echo "Flash logo error"; exit 1; fi
fastboot $* flash super `dirname $0`/images/super.img
if [ $? -ne 0 ] ; then echo "Flash super error"; exit 1; fi
fastboot $* flash userdata `dirname $0`/images/userdata.img
if [ $? -ne 0 ] ; then echo "Flash userdata error"; exit 1; fi
fastboot $* flash boot_ab `dirname $0`/images/boot.img
if [ $? -ne 0 ] ; then echo "Flash boot_a error"; exit 1; fi
fastboot $* flash vendor_boot_ab `dirname $0`/images/vendor_boot.img
if [ $? -ne 0 ] ; then echo "Flash vendor_boot_a error"; exit 1; fi
fastboot $* flash init_boot_ab `dirname $0`/images/init_boot.img
if [ $? -ne 0 ] ; then echo "flash init_boot_a error"; exit 1; fi
fastboot $* flash rescue `dirname $0`/images/rescue.img
if [ $? -ne 0 ] ; then echo "Flash rescue error"; exit 1; fi
fastboot $* flash countrycode_ab `dirname $0`/images/countrycode.img
if [ $? -ne 0 ] ; then echo "Flash countrycode error"; exit 1; fi
fastboot $* flash misc `dirname $0`/images/misc.img
if [ $? -ne 0 ] ; then echo "Flash misc error"; exit 1; fi
#fastboot $* flash cust `dirname $0`/images/cust.img
#if [ $? -ne 0 ] ; then echo "Flash cust error"; exit 1; fi
#fastboot $* flash oops `dirname $0`/images/oops.img
#if [ $? -ne 0 ] ; then echo "Flash oops error"; exit 1; fi
fastboot $* set_active a
#fastboot $* erase misc
if [ $? -ne 0 ] ; then echo "Erase misc error"; exit 1; fi
fastboot $* oem cdms
fastboot $* oem lks 2>&1 | grep -E "lks = *1$"
if [ $? -ne 0 ] ; then fastboot $* oem lock; fi
fastboot $* reboot
if [ $? -ne 0 ] ; then echo "Reboot error"; exit 1; fi
