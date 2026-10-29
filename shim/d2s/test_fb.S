.global _start
.text

_start:
    /* Exynos 9825 S-Boot kernel entry */
    /* 0xca000000 Framebuffer tabanı */
    ldr x0, =0xca000000
    
    /* 1440 * 3040 piksel * 4 byte (XRGB8888) = 17,510,400 byte */
    /* 0x010B2000 byte */
    ldr x1, =0x010B2000
    
    /* Saf Cyan/Mavi renk: 0x0000FFFF */
    ldr w2, =0x0000FFFF

fill_loop:
    str w2, [x0], #4
    subs x1, x1, #4
    b.ne fill_loop

deadlock:
    /* Ekrana rengi bastıktan sonra dur */
    wfi
    b deadlock
