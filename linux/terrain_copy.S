.text
.hidden TerrainCopyBridge
.globl TerrainCopyBridge
.type TerrainCopyBridge,@function
// Linux 35924 db6b10..db6b29: one row, rsi=source, rcx=destination,
// rdi=positive even byte count. Return rax=count, edx=last uint16 read.
// All other registers survive. The surrounding stock loop advances rows.
TerrainCopyBridge:
 push %rdi
 push %rsi
 push %rcx
 mov %rdi,%rax
 lea (%rsi,%rax),%rdx
 cmp %rdx,%rcx
 jae .Ldisjoint
 lea (%rcx,%rax),%rdx
 cmp %rdx,%rsi
 jae .Ldisjoint
 // Preserve the observable forward uint16 order for every overlapping row,
 // including odd byte offsets. Neither memcpy nor memmove is equivalent.
 pop %rcx
 pop %rsi
 pop %rdi
 xor %eax,%eax
.Lscalar:
 movzwl (%rsi,%rax),%edx
 mov %dx,(%rcx,%rax)
 add $2,%rax
 cmp %rax,%rdi
 jne .Lscalar
 jmp *bigmap_copy_return(%rip)
.Ldisjoint:
 movzwl -2(%rsi,%rax),%edx
 mov %rcx,%rdi
 mov %rax,%rcx
 shr $3,%rcx
 rep movsq
 mov %rax,%rcx
 and $7,%ecx
 shr $1,%ecx
 rep movsw
 pop %rcx
 pop %rsi
 pop %rdi
 jmp *bigmap_copy_return(%rip)
.size TerrainCopyBridge,.-TerrainCopyBridge
.hidden bigmap_copy_return
.section .note.GNU-stack,"",@progbits
