# ASUS Zenbook A16 (UX3607OA)
Notes about trying to run Linux on the Asus Zenbook A16. Note this is not something any sane person should do.

## Booting
### This applies to the version 312 of the BIOS/UEFI.

Press Esc at boot to get the UEFI boot menu.
F2 is the BIOS menu.
The  press has to be done before the boot logo has done its flare thing.
Just press repeatly until you get a boot menu.

Disabling Secure boot is an option in the UEFI menu. It is one of a handfull of options in the entire UEFI menu.

Boot order is *not* an option.

Boot order on this  Laptop is very quirky. Not sure if Insyde, Qualcomm, or ASUS is to blame.

What I have deduced:
1. Boot order is fixed to a specific order.
2. The UEFI will look for known files and paths in order to figure out which system to boot.
    * If it doesnt find anything it will default to EFI/BOOT/bootaa64.efi
    * When manually entering the boot menu (Esc) and if just one of the known efi files exist, 
        then you will not get the option to boot from EFI/BOOT/bootaa64.efi on that device.
3. The known paths and strings are:

|    Path                                |  Boot menu text         |      ??                 |
| -------------------------------------- | ----------------------- | ----------------------- |
|  \EFI\android\bootx64.efi              |  Android                |                         |
|  \EFI\fedora\shim.efi                  |  Fedora                 |                         |
|  \EFI\Microsoft\Boot\bootmgfw.efi      |  Windows Boot Manager   |                         |
|  \EFI\opensuse\grubx64.efi             |  openSUSE               |                         |
|  \EFI\redhat\grub.efi                  |  Red Hat Linux          |                         |
|  \EFI\SuSE\elilo.efi                   |  SuSE Linux             |                         |
|  \EFI\ubuntu\grub.efi                  |  ubuntu                 |  NORMAL                 |
|  \EFI\ubuntu\grub`$cpu$`.efi           |  ubuntu                 |  NORMAL                 |
|  \EFI\ubuntu\shim.efi                  |  ubuntu                 |  SECURE                 |
|  \EFI\ubuntu\shim`$cpu$`.efi           |  ubuntu                 |  SECURE                 |

`$cpu$`  seems to resolve to aa64.
\EFI\ubuntu\shimaa64.efi seems to go first. If  it exists then the system will boot that by default. Tested without Secure boot.
Windows seems to be the next. There may be an other option before Windows, but if  you want to  dual boot Windows and Linux you need to have a bootable file called `\efi\EFI\systemd\systemd-bootaa64.efi`. 
Just copy whatever your distro uses into that path.

## Kernel
Linux-next-20260813 kinda works but screen remains black.
Apply patch from msg fc3d0dfc-20cb-4289-a0dd-67ace50d42d8 {at} oss.qualcomm.com from the LKML to get the screen
working.

## Partition
The harddisk has ten firmware partitions followed by a EFI partition followed by Windows + Windows recovery. 
In short on the harddisk there were 16 original partitions. 
~~Do not touch the first ten partitions. Unknown how the machine would handle if they were modified or deleted.~~ 
~~(todo: try to boot without the ssd, see if the machine even boots).~~
Linux will boot without the first partitions.
Partition table is saved in the *info* folder.


## Firmware
Copy the update folder into your */lib/firmware* (or your firmware search path).
The files are in a folder called update folder so the board file is not overwritten laster by the package manager
Make sure the currect firmware is in /lib/firmware/ath12k/QCC2072/hw1.0.
Get the current firmware from the Linux Firmware Project.

To recreate:
Download the current driver pack from Asus' website and extract it by running the exe (WINE...?).
 
Find the file *bdwlan_qcc2072_1p0_ncm820A.elf* which was in the folder: *SOCPackage..../QualcommBSP/WIFI_BT/qcwlancol8480*

The board-2.bin was edited using the qca-swiss-army-knife using the following command:

    $ python3 ath12k --addboard board-2.bin.orig bdwlan_qcc2072_1p0_ncm820A.elf \
    'bus=pci,vendor=17cb,device=1112,subsystem-vendor=105b,subsystem-device=e14f,qmi-chip-id=33,qmi-board-id=255,variant=UX3407Q'\
    'bus=pci,vendor=17cb,device=1112,subsystem-vendor=105b,subsystem-device=e14f,qmi-chip-id=33,qmi-board-id=255'

Information about the card taken from dmesg on a running system:.

    [    7.476086] kernel: ath12k_wifi7_pci 0004:01:00.0: failed to fetch board data for 
    bus=pci,vendor=17cb,device=1112,subsystem-vendor=105b,subsystem-device=e14f,qmi-chip-id=33,qmi-board-id=255,variant=UX3407Q 
    from ath12k/QCC2072/hw1.0/board-2.bin

## Problems and thing that is not working

Oh boy. Screen not showing anything for a long time during boot. 
Sound  is a bit  flaky. Random reboots. RTC clock not working. USB reliability problems. 
Definately not fit for proper use yet, but the people at LKML are steadily improving. 
