/* Read-only ABI capture for the calculator's OS. Never opens a USB pipe or
 * writes a peripheral register. The small capture is firmware code only. */
#include <os.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define ADDRESS(nr, out) do { \
    register uintptr_t r0_ __asm__("r0"); \
    __asm__ __volatile__("swi %[n]" : "=r"(r0_) \
        : [n] "i"(__SYSCALLS_ISVAR | (nr)) \
        : "r1","r2","r3","r12","lr","memory","cc"); \
    out=r0_; \
} while (0)
int main(void)
{
    uintptr_t setup=0,transfer=0,open=0,error=0,status=0;
    uintptr_t addresses[5];
    const char *names[]={"setup_xfer_and_isoc","transfer","open_pipe","errstr","get_xfer_status"};
    const unsigned sizes[]={512,2048,512,512,512};
    char path[280];
    FILE *out;
    unsigned i,j;
    if (!nl_hassyscall(usbd_setup_xfer) || !nl_hassyscall(usbd_transfer) ||
        !nl_hassyscall(usbd_open_pipe) || !nl_hassyscall(usbd_errstr) ||
        !nl_hassyscall(usbd_get_xfer_status)) return 1;
    ADDRESS(232,setup); ADDRESS(229,transfer); ADDRESS(227,open);
    ADDRESS(254,error); ADDRESS(234,status);
    addresses[0]=setup; addresses[1]=transfer; addresses[2]=open;
    addresses[3]=error; addresses[4]=status;
    snprintf(path,sizeof path,"%susbaudio/usb_abi.txt.tns",get_documents_dir());
    out=fopen(path,"w");
    if (!out) { show_msgbox("USB check","Could not save the USB check."); return 1; }
    fprintf(out,"OS %08lx; CX II %d\n",(unsigned long)*(volatile uint32_t*)0x10000020u,is_cx2);
    for (i=0;i<5;i++) {
        uintptr_t at=addresses[i];
        if ((at&3) || at<0x10000000u || at>0x10fff800u) continue;
        fprintf(out,"%s %08lx\n",names[i],(unsigned long)at);
        for (j=0;j<sizes[i];j+=4)
            fprintf(out,"%08lx %08lx\n",(unsigned long)(at+j),
                    (unsigned long)*(volatile uint32_t*)(at+j));
    }
    /* This bound was verified in the CX II 6.4 errstr implementation. */
    if (!(error&3) && error>=0x10000000u && error<=0x10fff800u) {
        const char *(*errstr)(unsigned)=(void*)error;
        for (i=0;i<=20;i++) {
            const char *value=errstr(i);
            uintptr_t p=(uintptr_t)value;
            if (p<0x10000000u || p>0x10ffff80u) continue;
            fprintf(out,"status %u: %.96s\n",i,value);
        }
    }
    /* The live trace completes only 32 ms of audio per second. Capture
     * this OS's USB scheduler code for disassembly; never execute or
     * modify it. Addresses are gated to the three verified CX II 6.4
     * functions from earlier read-only captures. */
    if (is_cx2 && setup==0x10a07a34u && transfer==0x10a084e4u && open==0x10a08f4cu) {
        FILE *code;
        snprintf(path,sizeof path,"%susbaudio/usb_schedule.bin.tns",get_documents_dir());
        code=fopen(path,"wb");
        if (code) {
            size_t wrote=fwrite((const void *)0x109c0000u,1,0x80000u,code);
            int closed=fclose(code);
            fprintf(out,"scheduler code: base 109c0000 bytes %lu close %d\n",(unsigned long)wrote,closed);
        } else fprintf(out,"scheduler code: could not create capture\n");
        /* The scheduler picks a full-speed OUT descriptor's S-mask from a
         * byte table indexed by wMaxPacketSize / 188 (10a0de3c loads its
         * address, 10beab44, from 10a0e07c; the other controller's driver
         * uses 10bea868 from 10a0a270). The table is data outside the code
         * capture: read its first eight bytes. Same OS only (the gate
         * above); memory is read, nothing is written or called. */
        {
            static const uintptr_t tables[2]={0x10beab44u,0x10bea868u};
            for (i=0;i<2;i++) {
                const volatile uint8_t *table=(const volatile uint8_t *)tables[i];
                fprintf(out,"S-mask table %08lx:",(unsigned long)tables[i]);
                for (j=0;j<8;j++) fprintf(out," %02x",(unsigned)table[j]);
                fprintf(out,"\n");
            }
        }
    }
    if (fclose(out)) { show_msgbox("USB check","Could not finish saving the check."); return 1; }
    show_msgbox("USB check saved","Done. Press Enter, then reconnect the calculator to the computer.");
    return 0;
}
