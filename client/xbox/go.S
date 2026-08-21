/* client/xbox/go.S — jump to a loaded program (Xbox / i386).
 *
 * go(uint32_t addr) saves the loader's callee-saved registers and stack,
 * calls the loaded program at `addr`, and returns to the caller when the
 * program returns; the loader context is restored at go_return.
 */

    .code32
    .text
    .globl  go
    .globl  go_return

/* void go(uint32_t addr) — cdecl: addr at 4(%esp). */
go:
    /* Save callee-saved registers + esp so the loader's context can be
       restored when the program returns. */
    movl    %ebx, go_save_ebx
    movl    %esi, go_save_esi
    movl    %edi, go_save_edi
    movl    %ebp, go_save_ebp
    movl    %esp, go_save_esp

    movl    4(%esp), %eax           /* program entry address */
    call    *%eax                   /* run it; a plain ret comes back here    */

go_return:
    /* Restore the loader context. */
    movl    go_save_esp, %esp
    movl    go_save_ebx, %ebx
    movl    go_save_esi, %esi
    movl    go_save_edi, %edi
    movl    go_save_ebp, %ebp
    ret

    .bss
    .p2align 2
go_save_ebx:
    .space  4
go_save_esi:
    .space  4
go_save_edi:
    .space  4
go_save_ebp:
    .space  4
go_save_esp:
    .space  4
