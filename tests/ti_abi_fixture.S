/* Pure leaf functions captured read-only from a CX II OS 6.4.0.74.
 * These are position-independent ARM instructions, run by ti_abi_test under
 * ARM926 qemu. No USB device, peripheral access, or firmware call is made.
 * Keep the original instruction bytes: this fixture is independent evidence
 * for the C prototypes, zero lengths, and byte-sized status accessor.
 */
.syntax unified
.arm
.text
.align 2
.global ti_capture_setup_xfer
.type ti_capture_setup_xfer, %function
ti_capture_setup_xfer:
    .word 0xe92d0030 /* 10a07a34 */
    .word 0xe5d04044 /* 10a07a38 */
    .word 0xe580300c /* 10a07a3c */
    .word 0xe59d3008 /* 10a07a40 */
    .word 0xe1dd50bc /* 10a07a44 */
    .word 0xe5802004 /* 10a07a48 */
    .word 0xe5803010 /* 10a07a4c */
    .word 0xe59d2010 /* 10a07a50 */
    .word 0xe59d3014 /* 10a07a54 */
    .word 0xe3a0c000 /* 10a07a58 */
    .word 0xe3c44001 /* 10a07a5c */
    .word 0xe5801000 /* 10a07a60 */
    .word 0xe3a01001 /* 10a07a64 */
    .word 0xe580c038 /* 10a07a68 */
    .word 0xe1c051b8 /* 10a07a6c */
    .word 0xe580201c /* 10a07a70 */
    .word 0xe5c01020 /* 10a07a74 */
    .word 0xe5803024 /* 10a07a78 */
    .word 0xe5c04044 /* 10a07a7c */
    .word 0xe580c014 /* 10a07a80 */
    .word 0xe8bd0030 /* 10a07a84 */
    .word 0xe12fff1e /* 10a07a88 */
.size ti_capture_setup_xfer, .-ti_capture_setup_xfer
.global ti_capture_setup_isoc
.type ti_capture_setup_isoc, %function
ti_capture_setup_isoc:
    .word 0xe5802004 /* 10a07af8 */
    .word 0xe1dd20b8 /* 10a07afc */
    .word 0xe5801000 /* 10a07b00 */
    .word 0xe59d1004 /* 10a07b04 */
    .word 0xe5d0c044 /* 10a07b08 */
    .word 0xe3822004 /* 10a07b0c */
    .word 0xe1c021b8 /* 10a07b10 */
    .word 0xe3a02001 /* 10a07b14 */
    .word 0xe5801038 /* 10a07b18 */
    .word 0xe5c02020 /* 10a07b1c */
    .word 0xe59d100c /* 10a07b20 */
    .word 0xe59d2000 /* 10a07b24 */
    .word 0xe31c0008 /* 10a07b28 */
    .word 0x0580300c /* 10a07b2c */
    .word 0xe3ccc001 /* 10a07b30 */
    .word 0xe3a03000 /* 10a07b34 */
    .word 0xe580301c /* 10a07b38 */
    .word 0xe5801024 /* 10a07b3c */
    .word 0xe5c0c044 /* 10a07b40 */
    .word 0xe5802034 /* 10a07b44 */
    .word 0xe5803010 /* 10a07b48 */
    .word 0xe5803014 /* 10a07b4c */
    .word 0xe12fff1e /* 10a07b50 */
.size ti_capture_setup_isoc, .-ti_capture_setup_isoc
.global ti_capture_get_status
.type ti_capture_get_status, %function
ti_capture_get_status:
    .word 0xe3510000 /* 10a07b54 */
    .word 0xe52d4004 /* 10a07b58 */
    .word 0x15904004 /* 10a07b5c */
    .word 0xe59dc004 /* 10a07b60 */
    .word 0x15814000 /* 10a07b64 */
    .word 0xe3520000 /* 10a07b68 */
    .word 0x1590100c /* 10a07b6c */
    .word 0x15821000 /* 10a07b70 */
    .word 0xe3530000 /* 10a07b74 */
    .word 0x15902014 /* 10a07b78 */
    .word 0x15832000 /* 10a07b7c */
    .word 0xe35c0000 /* 10a07b80 */
    .word 0x15d03020 /* 10a07b84 */
    .word 0x15cc3000 /* 10a07b88 */
    .word 0xe8bd0010 /* 10a07b8c */
    .word 0xe12fff1e /* 10a07b90 */
.size ti_capture_get_status, .-ti_capture_get_status

/* Full captured transfer function. Tests provide its bus/pipe method tables
 * and exercise the accepted-descriptor, abort and bad-buffer paths only;
 * no branch to an external firmware function is reachable in those paths. */
.global ti_capture_transfer
.type ti_capture_transfer, %function
ti_capture_transfer:
    .word 0xe92d47f0 /* 10a084e4 */
    .word 0xe5905000 /* 10a084e8 */
    .word 0xe1a04000 /* 10a084ec */
    .word 0xe5953004 /* 10a084f0 */
    .word 0xe3a00000 /* 10a084f4 */
    .word 0xe5933000 /* 10a084f8 */
    .word 0xe5c40028 /* 10a084fc */
    .word 0xe5d52011 /* 10a08500 */
    .word 0xe3520000 /* 10a08504 */
    .word 0x0a000002 /* 10a08508 */
    .word 0xe3a07013 /* 10a0850c */
    .word 0xe1a00007 /* 10a08510 */
    .word 0xe8bd87f0 /* 10a08514 */
    .word 0xe1a00003 /* 10a08518 */
    .word 0xe5931008 /* 10a0851c */
    .word 0xe1a0e00f /* 10a08520 */
    .word 0xe591f018 /* 10a08524 */
    .word 0xe3500000 /* 10a08528 */
    .word 0x0afffff6 /* 10a0852c */
    .word 0xe5948000 /* 10a08530 */
    .word 0xe5946010 /* 10a08534 */
    .word 0xe5982008 /* 10a08538 */
    .word 0xe598e004 /* 10a0853c */
    .word 0xe5927000 /* 10a08540 */
    .word 0xe59e3000 /* 10a08544 */
    .word 0xe5d7c003 /* 10a08548 */
    .word 0xe20cc003 /* 10a0854c */
    .word 0xe35c0001 /* 10a08550 */
    .word 0x0a00005f /* 10a08554 */
    .word 0xe3560000 /* 10a08558 */
    .word 0x0a000016 /* 10a0855c */
    .word 0xe594200c /* 10a08560 */
    .word 0xe3520000 /* 10a08564 */
    .word 0x0a000078 /* 10a08568 */
    .word 0xe5d40044 /* 10a0856c */
    .word 0xe3100002 /* 10a08570 */
    .word 0x1a000010 /* 10a08574 */
    .word 0xe5927000 /* 10a08578 */
    .word 0xe3570000 /* 10a0857c */
    .word 0x0a000072 /* 10a08580 */
    .word 0xe594003c /* 10a08584 */
    .word 0xe9920082 /* 10a08588 */
    .word 0xe590e000 /* 10a0858c */
    .word 0xe3510000 /* 10a08590 */
    .word 0x03a01001 /* 10a08594 */
    .word 0x13a01003 /* 10a08598 */
    .word 0xe3570000 /* 10a0859c */
    .word 0x13811004 /* 10a085a0 */
    .word 0xe35c0001 /* 10a085a4 */
    .word 0xe5dec02d /* 10a085a8 */
    .word 0x020110fb /* 10a085ac */
    .word 0xe111000c /* 10a085b0 */
    .word 0x15842008 /* 10a085b4 */
    .word 0x0a000026 /* 10a085b8 */
    .word 0xe5981030 /* 10a085bc */
    .word 0xe5913000 /* 10a085c0 */
    .word 0xe3530000 /* 10a085c4 */
    .word 0x0a00001a /* 10a085c8 */
    .word 0xe1a00004 /* 10a085cc */
    .word 0xe12fff33 /* 10a085d0 */
    .word 0xe2507000 /* 10a085d4 */
    .word 0x0a000016 /* 10a085d8 */
    .word 0xe3570002 /* 10a085dc */
    .word 0x0a000002 /* 10a085e0 */
    .word 0xe1a00004 /* 10a085e4 */
    .word 0xe3a01001 /* 10a085e8 */
    .word 0xebfffe4e /* 10a085ec */
    .word 0xe1d4e1b8 /* 10a085f0 */
    .word 0xe31e0002 /* 10a085f4 */
    .word 0x0affffc4 /* 10a085f8 */
    .word 0xe3570002 /* 10a085fc */
    .word 0x1affffc2 /* 10a08600 */
    .word 0xe5d40028 /* 10a08604 */
    .word 0xe3500000 /* 10a08608 */
    .word 0x1a000007 /* 10a0860c */
    .word 0xe5952004 /* 10a08610 */
    .word 0xe5923000 /* 10a08614 */
    .word 0xe5d37234 /* 10a08618 */
    .word 0xe3570000 /* 10a0861c */
    .word 0x1a00004c /* 10a08620 */
    .word 0xe594005c /* 10a08624 */
    .word 0xe3a01000 /* 10a08628 */
    .word 0xebea037e /* 10a0862c */
    .word 0xe5d47020 /* 10a08630 */
    .word 0xeaffffb5 /* 10a08634 */
    .word 0xe1a00004 /* 10a08638 */
    .word 0xebfffdc8 /* 10a0863c */
    .word 0xe2507000 /* 10a08640 */
    .word 0x1affffe4 /* 10a08644 */
    .word 0xe1a00008 /* 10a08648 */
    .word 0xebfffe8a /* 10a0864c */
    .word 0xe3a07002 /* 10a08650 */
    .word 0xeaffffe5 /* 10a08654 */
    .word 0xe1a00003 /* 10a08658 */
    .word 0xe2841040 /* 10a0865c */
    .word 0xe5933008 /* 10a08660 */
    .word 0xe1a02006 /* 10a08664 */
    .word 0xe1a0e00f /* 10a08668 */
    .word 0xe593f008 /* 10a0866c */
    .word 0xe2507000 /* 10a08670 */
    .word 0x1affffa5 /* 10a08674 */
    .word 0xe5940040 /* 10a08678 */
    .word 0xeb003772 /* 10a0867c */
    .word 0xe5d42044 /* 10a08680 */
    .word 0xe3828002 /* 10a08684 */
    .word 0xe3180001 /* 10a08688 */
    .word 0xe5c48044 /* 10a0868c */
    .word 0x05948000 /* 10a08690 */
    .word 0x15d48029 /* 10a08694 */
    .word 0x12088080 /* 10a08698 */
    .word 0xe5840008 /* 10a0869c */
    .word 0x05988008 /* 10a086a0 */
    .word 0x05988000 /* 10a086a4 */
    .word 0x05d88002 /* 10a086a8 */
    .word 0x02088080 /* 10a086ac */
    .word 0xe3580000 /* 10a086b0 */
    .word 0x15948000 /* 10a086b4 */
    .word 0x1affffbf /* 10a086b8 */
    .word 0xe594c00c /* 10a086bc */
    .word 0xe5900000 /* 10a086c0 */
    .word 0xe59c1000 /* 10a086c4 */
    .word 0xe1a02006 /* 10a086c8 */
    .word 0xebea031a /* 10a086cc */
    .word 0xe5948000 /* 10a086d0 */
    .word 0xeaffffb8 /* 10a086d4 */
    .word 0xe5947038 /* 10a086d8 */
    .word 0xe3570000 /* 10a086dc */
    .word 0x01a06007 /* 10a086e0 */
    .word 0x0a000017 /* 10a086e4 */
    .word 0xe5940034 /* 10a086e8 */
    .word 0xe2476001 /* 10a086ec */
    .word 0xe15c0007 /* 10a086f0 */
    .word 0xe2061001 /* 10a086f4 */
    .word 0xe1a0200c /* 10a086f8 */
    .word 0xe1d060b0 /* 10a086fc */
    .word 0x2a000010 /* 10a08700 */
    .word 0xe3510000 /* 10a08704 */
    .word 0x0a000004 /* 10a08708 */
    .word 0xe3a02002 /* 10a0870c */
    .word 0xe19010b2 /* 10a08710 */
    .word 0xe1520007 /* 10a08714 */
    .word 0xe0866001 /* 10a08718 */
    .word 0x2a000009 /* 10a0871c */
    .word 0xe1a0a082 /* 10a08720 */
    .word 0xe282e001 /* 10a08724 */
    .word 0xe19090ba /* 10a08728 */
    .word 0xe1a0208e /* 10a0872c */
    .word 0xe190a0b2 /* 10a08730 */
    .word 0xe28e2001 /* 10a08734 */
    .word 0xe0861009 /* 10a08738 */
    .word 0xe1520007 /* 10a0873c */
    .word 0xe081600a /* 10a08740 */
    .word 0x3afffff5 /* 10a08744 */
    .word 0xe5846010 /* 10a08748 */
    .word 0xeaffff81 /* 10a0874c */
    .word 0xe3a07012 /* 10a08750 */
    .word 0xeaffff6d /* 10a08754 */
    .word 0xe59f0004 /* 10a08758 */
    .word 0xebea00c7 /* 10a0875c */
    .word 0xeaffffaf /* 10a08760 */
    .word 0x10bea850 /* 10a08764 */
.size ti_capture_transfer, .-ti_capture_transfer
