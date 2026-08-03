# ASUS Zenbook A16 (UX3607OA)
Notes about trying to run Linux on the Asus Zenbook A16. Note this is not something any sane person should do.

## Kernel
Linux-next-20260803 kinda works but screen remains black.
Apply patch from msg fc3d0dfc-20cb-4289-a0dd-67ace50d42d8 {at} oss.qualcomm.com from the LKML to get the screen
working.

## Partition
The harddisk has ten firmware partitions followed by a EFI partition followed by Windows + Windows recovery. 
In short on the harddisk there were 16 original partitions. 
Do not touch the first ten partitions. Unknown how the machine would handle if they were modified or deleted. 
(todo: try to boot without the ssd, see if the machine even boots).
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

