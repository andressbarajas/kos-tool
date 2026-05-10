#ifndef PS2_IOP_DEV9_INIT_SHARED_H
#define PS2_IOP_DEV9_INIT_SHARED_H

struct iop_mailbox {
    volatile unsigned int cmd;
    volatile int result;
    volatile unsigned int rev;
    volatile unsigned int power;
    volatile unsigned int map;
    volatile unsigned int presence;
};

int dev9_init_run(struct iop_mailbox *mbox);

#endif /* PS2_IOP_DEV9_INIT_SHARED_H */
