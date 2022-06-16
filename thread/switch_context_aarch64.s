/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

.p2align 4,,15
.globl _photon_switch_context
#if !defined( __APPLE__ ) && !defined( __FreeBSD__ )
.type  _photon_switch_context, @function
#endif
_photon_switch_context: //(void** from, void** to)

stp x19, x20, [sp, #-0x10] // x means 64-bit general register
stp x21, x22, [sp, #-0x20]
stp x23, x24, [sp, #-0x30]
stp x25, x26, [sp, #-0x40]
stp x27, x28, [sp, #-0x50]
stp x29, x30, [sp, #-0x60]
sub sp, sp, #0x60
mov x4, sp // r4 = sp
str x4, [x0] // (*rdi_from_c) = r4
ldr x4, [x1] // sp = (*rsi_to_c)
mov sp, x4
ldp x29, x30, [sp, #0x00]
ldp x27, x28, [sp, #0x10]
ldp x25, x26, [sp, #0x20]
ldp x23, x24, [sp, #0x30]
ldp x21, x22, [sp, #0x40]
ldp x19, x20, [sp, #0x50]
add sp, sp, #0x60
ret



.globl _photon_switch_context_defer
#if !defined( __APPLE__ ) && !defined( __FreeBSD__ )
.type  _photon_switch_context_defer, @function
#endif
_photon_switch_context_defer: //(void** rdi_from_c, void** rsi_to_c,
                              //    void (*rdx_defer)(void*), void* rcx_arg)

stp x19, x20, [sp, #-0x10] // x means 64-bit general register
stp x21, x22, [sp, #-0x20]
stp x23, x24, [sp, #-0x30]
stp x25, x26, [sp, #-0x40]
stp x27, x28, [sp, #-0x50]
stp x29, x30, [sp, #-0x60]
sub sp, sp, #0x60
mov x4, sp // r5 = sp
str x4, [x0] // (*rdi_from_c) = r5
mov x0, x3 // let x0 = rcx_arg

.globl _photon_switch_context_defer_die
#if !defined( __APPLE__ ) && !defined( __FreeBSD__ )
.type  _photon_switch_context_defer_die, @function
#endif
_photon_switch_context_defer_die:   // (void* rdi_dyting_th, void** rsi_to_c,
                                    //  void (*rdx_defer_die)())
ldr x4, [x1] // sp = (*rsi_to_c)
mov sp, x4
// sub sp, sp, #0x10
blr x2
ldp x19, x20, [sp, #0x50]
ldp  x21, x22, [sp, #0x40]
ldp  x23, x24, [sp, #0x30]
ldp  x25, x26, [sp, #0x20]
ldp  x27, x28, [sp, #0x10]
ldp  x29, x30, [sp, #0x00]
add sp, sp, #0x60
ret
