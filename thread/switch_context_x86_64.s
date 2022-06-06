.globl _photon_switch_context
#if !defined( __APPLE__ ) && !defined( __FreeBSD__ )
.type  _photon_switch_context, @function
#endif
_photon_switch_context: //(void** from, void** to)

mov %rbx, -8(%rsp);
mov %rbp, -16(%rsp);
mov %r12, -24(%rsp);
mov %r13, -32(%rsp);
mov %r14, -40(%rsp);
mov %r15, -48(%rsp);
leaq  -48(%rsp), %rsp
mov  %rsp, (%rdi);   // rdi is `from`
mov  (%rsi), %rsp;   // rsi is `to`
mov 0(%rsp), %r15;
mov 8(%rsp), %r14;
mov 16(%rsp), %r13;
mov 24(%rsp), %r12;
mov 32(%rsp), %rbp;
mov 40(%rsp), %rbx;
leaq 48(%rsp), %rsp;
ret;



.globl _photon_switch_context_defer
#if !defined( __APPLE__ ) && !defined( __FreeBSD__ )
.type  _photon_switch_context_defer, @function
#endif
_photon_switch_context_defer: //(void** rdi_from_c, void** rsi_to_c,
                              //    void (*rdx_defer)(void*), void* rcx_arg)

mov %rbx, -8(%rsp);
mov %rbp, -16(%rsp);
mov %r12, -24(%rsp);
mov %r13, -32(%rsp);
mov %r14, -40(%rsp);
mov %r15, -48(%rsp);
leaq  -48(%rsp), %rsp;
mov %rsp, (%rdi);
mov %rcx, %rdi; // and continues with the
                // following code segment
.globl _photon_switch_context_defer_die
#if !defined( __APPLE__ ) && !defined( __FreeBSD__ )
.type  _photon_switch_context_defer_die, @function
#endif
_photon_switch_context_defer_die:   // (void* rdi_dyting_th, void** rsi_to_c,
                                    //  void (*rdx_defer_die)())
mov (%rsi), %rsp;
leaq -8(%rsp), %rsp; // switch to `to` ctx
call *%rdx;          // and call defer(arg)
mov 8(%rsp), %r15;
mov 16(%rsp), %r14;
mov 24(%rsp), %r13;
mov 32(%rsp), %r12;
mov 40(%rsp), %rbp;
mov 48(%rsp), %rbx;
leaq 56(%rsp), %rsp;
ret;
