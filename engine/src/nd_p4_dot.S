/* int32_t nd_p4_dot128_s8(const int8_t *a, const int8_t *b)
 *
 * 128 signed int8 multiply-adds on the ESP32-P4 PIE unit. The C side keeps the
 * scalar fallback; this file is linked only for esp32p4. The instruction
 * pattern follows esp-dsp's modules/dotprod/fixed/dspi_dotprod_s8_arp4.S.
 *
 * The result goes to a0 because esp.movx.r.xacc.l only accepts a restricted
 * destination register class; writing the whole kernel in assembly avoids
 * depending on how GCC allocates the inline-asm operand.
 */
    .section .iram1.nd_p4_dot, "ax"
    .align 4
    .global nd_p4_dot128_s8
    .type   nd_p4_dot128_s8, @function

nd_p4_dot128_s8:
    /* a0 = a, a1 = b */
    esp.zero.xacc
    esp.vld.128.ip  q0, a0, 16
    esp.vld.128.ip  q1, a1, 16
    esp.vmulas.s8.xacc q0, q1
    esp.vld.128.ip  q2, a0, 16
    esp.vld.128.ip  q3, a1, 16
    esp.vmulas.s8.xacc q2, q3
    esp.vld.128.ip  q4, a0, 16
    esp.vld.128.ip  q5, a1, 16
    esp.vmulas.s8.xacc q4, q5
    esp.vld.128.ip  q6, a0, 16
    esp.vld.128.ip  q7, a1, 16
    esp.vmulas.s8.xacc q6, q7
    esp.vld.128.ip  q0, a0, 16
    esp.vld.128.ip  q1, a1, 16
    esp.vmulas.s8.xacc q0, q1
    esp.vld.128.ip  q2, a0, 16
    esp.vld.128.ip  q3, a1, 16
    esp.vmulas.s8.xacc q2, q3
    esp.vld.128.ip  q4, a0, 16
    esp.vld.128.ip  q5, a1, 16
    esp.vmulas.s8.xacc q4, q5
    esp.vld.128.ip  q6, a0, 16
    esp.vld.128.ip  q7, a1, 16
    esp.vmulas.s8.xacc q6, q7
    esp.movx.r.xacc.l a0
    ret
    .size nd_p4_dot128_s8, . - nd_p4_dot128_s8
