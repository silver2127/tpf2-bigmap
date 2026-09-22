.text
.globl TestTerrainCopy
.type TestTerrainCopy,@function
// src, dst, bytes, entry, output registers
TestTerrainCopy:
 push %rbx
 push %r12
 mov %r8,%r12
 mov %rcx,%rbx
 mov %rsi,%rcx
 mov %rdi,%rsi
 mov %rdx,%rdi
 mov $123,%r8
 mov $234,%r9
 mov $345,%r10
 mov $456,%r11
 lea TestTerrainCopyDone(%rip),%rax
 mov %rax,bigmap_copy_return(%rip)
 jmp *%rbx
.globl TestTerrainCopyDone
TestTerrainCopyDone:
 mov %rax,0(%r12)
 mov %rdx,8(%r12)
 mov %rsi,16(%r12)
 mov %rcx,24(%r12)
 mov %rdi,32(%r12)
 mov %r8,40(%r12)
 mov %r9,48(%r12)
 mov %r10,56(%r12)
 mov %r11,64(%r12)
 pop %r12
 pop %rbx
 ret
.section .note.GNU-stack,"",@progbits
