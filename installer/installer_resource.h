#pragma once
// Resource IDs for PSCam4Win-Setup.exe (installer.rc).

// Icons
#define IDI_SETUP               100

// Dialogs
#define IDD_SETUP_FRAME         200
#define IDD_PAGE_WELCOME        210
#define IDD_PAGE_LICENSE        220
#define IDD_PAGE_COMPONENTS     230
#define IDD_PAGE_PROGRESS       240
#define IDD_PAGE_FINISH         250
#define IDD_PAGE_MAINTENANCE    260
#define IDD_PAGE_UNCONFIRM      270

// Frame controls
#define IDC_HEADER_TITLE        300
#define IDC_HEADER_SUB          301
#define IDC_BACK                302
#define IDC_NEXT                303   // caption morphs: "Next >" / "Install" / "Uninstall" / "Finish"

// Welcome page
#define IDC_WELCOME_TEXT        310

// License page
#define IDC_LICENSE_EDIT        320
#define IDC_LICENSE_ACCEPT      321

// Components page
#define IDC_COMP_PS3            330
#define IDC_COMP_EYETOY         331
#define IDC_COMP_PS4            332
#define IDC_OPT_AUTOSTART       333
#define IDC_OPT_LAUNCH          334
#define IDC_COMP_PATH           335
#define IDC_COMP_NOTE           336

// Progress page
#define IDC_PROGRESS_BAR        340
#define IDC_PROGRESS_LOG        341
#define IDC_PROGRESS_STEP       342

// Finish page
#define IDC_FINISH_TEXT         350
#define IDC_FINISH_REBOOT       351

// Maintenance page
#define IDC_MAINT_TEXT          360
#define IDC_MAINT_REINSTALL     361
#define IDC_MAINT_UNINSTALL     362

// Uninstall-confirm page
#define IDC_UN_TEXT             370
#define IDC_UN_REMOVEDATA       371

// RCDATA payload (raw, uncompressed)
#define IDR_PAYLOAD_DLL         500   // build\PSCam4Win.dll
#define IDR_PAYLOAD_TRAY        501   // build\PSCam4WinTray.exe
#define IDR_LICENSE             502   // LICENSE (GPLv2 text)
#define IDR_INF_PS3             510   // driver\usb_device.inf
#define IDR_CAT_PS3             511
#define IDR_CER_PS3             512
#define IDR_INF_EYETOY          513   // driver\eyetoy_device.inf
#define IDR_CAT_EYETOY          514
#define IDR_CER_EYETOY          515
#define IDR_INF_PS4             516   // driver\ps4cam_device.inf
#define IDR_CAT_PS4             517
#define IDR_CER_PS4             518
