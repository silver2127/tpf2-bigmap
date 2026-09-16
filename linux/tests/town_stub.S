.text
.globl TestTown
.type TestTown,@function
TestTown:
 push %rbp
 sub $512,%rsp
 lea 512(%rsp),%rbp
 mov %esi,%eax
 call *%rdi
 movss -444(%rbp),%xmm0
 add $512,%rsp
 pop %rbp
 ret
.section .note.GNU-stack,"",@progbits
