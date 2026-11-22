/* Execute the captured OS leaf functions, not a reimplementation. */
#include <stdint.h>
#include <string.h>
#include "t.h"

extern void ti_capture_setup_xfer(void *, void *, void *, void *, uint32_t,
                                   unsigned, uint32_t, void *);
extern void ti_capture_setup_isoc(void *, void *, void *, void *, uint16_t *,
                                   uint32_t, unsigned, void *);
extern void ti_capture_get_status(void *, void **, void **, uint32_t *, uint8_t *);
extern unsigned ti_capture_transfer(void *);

static unsigned submitted;
static int bus_active(void *bus) { (void)bus; return 1; }
static unsigned submit(void *transfer)
{
    uint32_t *xfer = transfer;
    /* Reached only after the real transfer code sums the frame array and
     * interprets its independent three-word buffer descriptor. */
    T_EQ(xfer[4], 1536);
    T_EQ(xfer[2], xfer[3]);
    T_EQ(xfer[14], 8);
    submitted++;
    return 2;
}

int main(void)
{
    uint32_t xfer[32] = {0}, data[4] = {0}, *descriptor[3] = {data, 0, 0};
    uint16_t lengths[8] = {192,192,192,192,192,192,192,192};
    uint8_t status[3] = {0xA5,0xFF,0x5A};
    void *priv = 0, *buffer = 0;
    uint32_t count = UINT32_MAX;
    void *pipe = (void *)0x12340, *cookie = (void *)0x56780, *cb = (void *)0x9ABC0;

    ti_capture_setup_xfer(xfer,pipe,cookie,descriptor,1536,0,999,cb);
    T_EQ(xfer[3], (uintptr_t)descriptor);
    T_EQ(xfer[4], 1536);
    T_EQ(xfer[7], 999);
    T_EQ(xfer[14], 0);
    ti_capture_setup_isoc(xfer,pipe,cookie,descriptor,lengths,8,0,cb);
    T_EQ(xfer[0], (uintptr_t)pipe);
    T_EQ(xfer[1], (uintptr_t)cookie);
    T_EQ(xfer[3], (uintptr_t)descriptor);
    T_EQ(xfer[4], 0); /* sum is computed later, inside usbd_transfer */
    T_EQ(xfer[5], 0);
    T_EQ(xfer[6] & 0xFFFF, 4);
    T_EQ(xfer[7], 0);
    T_EQ(xfer[8] & 0xFF, 1);
    T_EQ(xfer[9], (uintptr_t)cb);
    T_EQ(xfer[13], (uintptr_t)lengths);
    T_EQ(xfer[14], 8);

    ti_capture_get_status(xfer,&priv,&buffer,&count,&status[1]);
    T_EQ((uintptr_t)priv, (uintptr_t)cookie);
    T_EQ((uintptr_t)buffer, (uintptr_t)descriptor);
    T_EQ(count, 0);
    T_EQ(status[0], 0xA5);
    T_EQ(status[1], 1);
    T_EQ(status[2], 0x5A); /* only a byte is written */

    {
        uint32_t bus[16] = {0}, methods[16] = {0}, device[16] = {0};
        uint32_t real_pipe[32] = {0}, pipe_methods[8] = {0}, endpoint[4] = {0};
        uint32_t dma_tag[4] = {0};
        uint8_t dma_methods[64] = {0}, ep_desc[8] = {7,5,1,1,200,0,1,0};
        bus[2] = (uintptr_t)methods;
        methods[6] = (uintptr_t)bus_active;
        device[0] = (uintptr_t)bus;
        endpoint[0] = (uintptr_t)ep_desc;
        real_pipe[1] = (uintptr_t)device;
        real_pipe[2] = (uintptr_t)endpoint;
        real_pipe[12] = (uintptr_t)pipe_methods;
        pipe_methods[0] = (uintptr_t)submit;
        dma_tag[0] = (uintptr_t)dma_methods;
        dma_methods[45] = 1; /* this bus accepts a plain virtual descriptor */
        ti_capture_setup_isoc(xfer,real_pipe,cookie,descriptor,lengths,8,0,cb);
        xfer[15] = (uintptr_t)dma_tag;
        T_EQ(ti_capture_transfer(xfer), 2);
        T_EQ(submitted, 1);
        T_EQ(xfer[4], 1536);
        ((uint8_t *)real_pipe)[17] = 1; /* pipe is aborting */
        T_EQ(ti_capture_transfer(xfer), 19);
        T_EQ(submitted, 1);
        ((uint8_t *)real_pipe)[17] = 0;
        xfer[3] = 0; /* no descriptor: INVALID PARAMETER */
        T_EQ(ti_capture_transfer(xfer), 18);
        T_EQ(submitted, 1);
    }

    ti_capture_setup_xfer(xfer,pipe,cookie,descriptor,1536,0,0,cb);
    T_EQ(xfer[14], 0); /* the former two-call workaround destroys isoc setup */
    return T_DONE();
}
