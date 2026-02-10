	.arch armv7-a
	.eabi_attribute 20, 1	@ Tag_ABI_FP_denormal
	.eabi_attribute 21, 1	@ Tag_ABI_FP_exceptions
	.eabi_attribute 23, 3	@ Tag_ABI_FP_number_model
	.eabi_attribute 24, 1	@ Tag_ABI_align8_needed
	.eabi_attribute 25, 1	@ Tag_ABI_align8_preserved
	.eabi_attribute 26, 2	@ Tag_ABI_enum_size
	.eabi_attribute 30, 4	@ Tag_ABI_optimization_goals
	.eabi_attribute 34, 0	@ Tag_CPU_unaligned_access
	.eabi_attribute 18, 2	@ Tag_ABI_PCS_wchar_t
	.file	"asm-offsets.c"
@ GNU C11 (Ubuntu 9.4.0-1ubuntu1~20.04.2) version 9.4.0 (arm-linux-gnueabihf)
@	compiled by GNU C version 9.4.0, GMP version 6.2.0, MPFR version 4.0.2, MPC version 1.1.0, isl version isl-0.22.1-GMP

@ GGC heuristics: --param ggc-min-expand=100 --param ggc-min-heapsize=131072
@ options passed:  -nostdinc -I include -I ./arch/arm/include -imultilib sf
@ -imultiarch arm-linux-gnueabi -D __KERNEL__ -D __UBOOT__ -D __ARM__
@ -D __LINUX_ARM_ARCH__=7 -D DO_DEPS_ONLY -D KBUILD_BASENAME="asm_offsets"
@ -D KBUILD_MODNAME="asm_offsets"
@ -isystem /usr/lib/gcc-cross/arm-linux-gnueabihf/9/include
@ -include ./include/linux/kconfig.h -MD arch/arm/lib/.asm-offsets.s.d
@ arch/arm/lib/asm-offsets.c -marm -mno-thumb-interwork -mabi=aapcs-linux
@ -mword-relocations -mno-unaligned-access -mfloat-abi=soft
@ -mgeneral-regs-only -mtune=generic-armv7-a -mfpu=vfpv3-d16
@ -mtls-dialect=gnu -march=armv7-a
@ -auxbase-strip arch/arm/lib/asm-offsets.s -gdwarf-4 -Os -Wall
@ -Wstrict-prototypes -Wno-format-security -Wno-pointer-sign
@ -Wno-stringop-truncation -Wno-array-bounds -Wstringop-overflow=0
@ -Wno-maybe-uninitialized -Wno-format-nonliteral
@ -Wno-address-of-packed-member -Wno-unused-but-set-variable
@ -Werror=date-time -Wno-packed-not-aligned -std=gnu11 -fno-builtin
@ -ffreestanding -fshort-wchar -fno-strict-aliasing -fno-stack-protector
@ -fno-delete-null-pointer-checks -fmacro-prefix-map=./= -fstack-usage
@ -fno-pic -ffunction-sections -fdata-sections -fno-common -ffixed-r9
@ -fverbose-asm
@ options enabled:  -faggressive-loop-optimizations -fassume-phsa
@ -fauto-inc-dec -fbranch-count-reg -fcaller-saves -fcode-hoisting
@ -fcombine-stack-adjustments -fcompare-elim -fcprop-registers
@ -fcrossjumping -fcse-follow-jumps -fdata-sections -fdefer-pop
@ -fdevirtualize -fdevirtualize-speculatively -fdwarf2-cfi-asm
@ -fearly-inlining -feliminate-unused-debug-types -fexpensive-optimizations
@ -fforward-propagate -ffp-int-builtin-inexact -ffunction-cse
@ -ffunction-sections -fgcse -fgcse-lm -fgnu-runtime -fgnu-unique
@ -fguess-branch-probability -fhoist-adjacent-loads -fident -fif-conversion
@ -fif-conversion2 -findirect-inlining -finline -finline-atomics
@ -finline-functions -finline-functions-called-once
@ -finline-small-functions -fipa-bit-cp -fipa-cp -fipa-icf
@ -fipa-icf-functions -fipa-icf-variables -fipa-profile -fipa-pure-const
@ -fipa-ra -fipa-reference -fipa-reference-addressable -fipa-sra
@ -fipa-stack-alignment -fipa-vrp -fira-hoist-pressure
@ -fira-share-save-slots -fira-share-spill-slots
@ -fisolate-erroneous-paths-dereference -fivopts -fkeep-static-consts
@ -fleading-underscore -flifetime-dse -flra-remat -flto-odr-type-merging
@ -fmath-errno -fmerge-constants -fmerge-debug-strings
@ -fmove-loop-invariants -fomit-frame-pointer -foptimize-sibling-calls
@ -fpartial-inlining -fpeephole -fpeephole2 -fplt -fprefetch-loop-arrays
@ -freg-struct-return -freorder-blocks -freorder-functions
@ -frerun-cse-after-loop -fsched-critical-path-heuristic
@ -fsched-dep-count-heuristic -fsched-group-heuristic -fsched-interblock
@ -fsched-last-insn-heuristic -fsched-pressure -fsched-rank-heuristic
@ -fsched-spec -fsched-spec-insn-heuristic -fsched-stalled-insns-dep
@ -fschedule-insns2 -fsection-anchors -fsemantic-interposition
@ -fshow-column -fshrink-wrap -fshrink-wrap-separate -fsigned-zeros
@ -fsplit-ivs-in-unroller -fsplit-wide-types -fssa-backprop -fssa-phiopt
@ -fstdarg-opt -fstore-merging -fstrict-volatile-bitfields -fsync-libcalls
@ -fthread-jumps -ftoplevel-reorder -ftrapping-math -ftree-bit-ccp
@ -ftree-builtin-call-dce -ftree-ccp -ftree-ch -ftree-coalesce-vars
@ -ftree-copy-prop -ftree-cselim -ftree-dce -ftree-dominator-opts
@ -ftree-dse -ftree-forwprop -ftree-fre -ftree-loop-if-convert
@ -ftree-loop-im -ftree-loop-ivcanon -ftree-loop-optimize
@ -ftree-parallelize-loops= -ftree-phiprop -ftree-pre -ftree-pta
@ -ftree-reassoc -ftree-scev-cprop -ftree-sink -ftree-slsr -ftree-sra
@ -ftree-switch-conversion -ftree-tail-merge -ftree-ter -ftree-vrp
@ -funit-at-a-time -fvar-tracking -fvar-tracking-assignments -fverbose-asm
@ -fzero-initialized-in-bss -marm -mbe32 -mgeneral-regs-only -mglibc
@ -mlittle-endian -mpic-data-is-text-relative -msched-prolog
@ -mvectorize-with-neon-quad -mword-relocations

	.text
.Ltext0:
	.cfi_sections	.debug_frame
	.section	.text.startup.main,"ax",%progbits
	.align	2
	.global	main
	.syntax unified
	.arm
	.fpu softvfp
	.type	main, %function
main:
.LFB99:
	.file 1 "arch/arm/lib/asm-offsets.c"
	.loc 1 23 1 view -0
	.cfi_startproc
	@ args = 0, pretend = 0, frame = 0
	@ frame_needed = 0, uses_anonymous_args = 0
	@ link register save eliminated.
	.loc 1 95 2 view .LVU1
@ arch/arm/lib/asm-offsets.c:96: }
	.loc 1 96 1 is_stmt 0 view .LVU2
	mov	r0, #0	@,
	bx	lr	@
	.cfi_endproc
.LFE99:
	.size	main, .-main
	.text
.Letext0:
	.file 2 "include/errno.h"
	.file 3 "include/linux/string.h"
	.file 4 "./arch/arm/include/asm/u-boot-arm.h"
	.file 5 "include/env_internal.h"
	.file 6 "include/linux/types.h"
	.section	.debug_info,"",%progbits
.Ldebug_info0:
	.4byte	0x154
	.2byte	0x4
	.4byte	.Ldebug_abbrev0
	.byte	0x4
	.uleb128 0x1
	.4byte	.LASF22
	.byte	0xc
	.4byte	.LASF23
	.4byte	.LASF24
	.4byte	.Ldebug_ranges0+0
	.4byte	0
	.4byte	.Ldebug_line0
	.uleb128 0x2
	.4byte	.LASF10
	.byte	0x2
	.byte	0x11
	.byte	0xc
	.4byte	0x31
	.uleb128 0x3
	.byte	0x4
	.byte	0x5
	.ascii	"int\000"
	.uleb128 0x4
	.4byte	0x5b
	.4byte	0x48
	.uleb128 0x5
	.4byte	0x4d
	.byte	0
	.byte	0
	.uleb128 0x6
	.4byte	0x38
	.uleb128 0x7
	.byte	0x4
	.byte	0x7
	.4byte	.LASF0
	.uleb128 0x7
	.byte	0x1
	.byte	0x8
	.4byte	.LASF1
	.uleb128 0x6
	.4byte	0x54
	.uleb128 0x8
	.4byte	.LASF25
	.byte	0x2
	.byte	0x1f
	.byte	0x13
	.4byte	0x48
	.ascii	"\000"
	.uleb128 0x7
	.byte	0x4
	.byte	0x7
	.4byte	.LASF2
	.uleb128 0x7
	.byte	0x2
	.byte	0x7
	.4byte	.LASF3
	.uleb128 0x7
	.byte	0x4
	.byte	0x5
	.4byte	.LASF4
	.uleb128 0x9
	.byte	0x4
	.4byte	0x54
	.uleb128 0x7
	.byte	0x8
	.byte	0x5
	.4byte	.LASF5
	.uleb128 0x7
	.byte	0x1
	.byte	0x6
	.4byte	.LASF6
	.uleb128 0x7
	.byte	0x1
	.byte	0x8
	.4byte	.LASF7
	.uleb128 0x7
	.byte	0x2
	.byte	0x5
	.4byte	.LASF8
	.uleb128 0x7
	.byte	0x8
	.byte	0x7
	.4byte	.LASF9
	.uleb128 0xa
	.4byte	.LASF26
	.byte	0x6
	.byte	0x5b
	.byte	0x18
	.4byte	0x6d
	.uleb128 0x2
	.4byte	.LASF11
	.byte	0x3
	.byte	0xb
	.byte	0xf
	.4byte	0x82
	.uleb128 0x2
	.4byte	.LASF12
	.byte	0x4
	.byte	0x12
	.byte	0xe
	.4byte	0xab
	.uleb128 0x2
	.4byte	.LASF13
	.byte	0x4
	.byte	0x13
	.byte	0xe
	.4byte	0xab
	.uleb128 0x2
	.4byte	.LASF14
	.byte	0x4
	.byte	0x14
	.byte	0xe
	.4byte	0xab
	.uleb128 0x2
	.4byte	.LASF15
	.byte	0x4
	.byte	0x15
	.byte	0xe
	.4byte	0xab
	.uleb128 0x2
	.4byte	.LASF16
	.byte	0x4
	.byte	0x16
	.byte	0xe
	.4byte	0xab
	.uleb128 0x2
	.4byte	.LASF17
	.byte	0x4
	.byte	0x17
	.byte	0xe
	.4byte	0xab
	.uleb128 0x2
	.4byte	.LASF18
	.byte	0x4
	.byte	0x18
	.byte	0xe
	.4byte	0xab
	.uleb128 0x7
	.byte	0x8
	.byte	0x4
	.4byte	.LASF19
	.uleb128 0x4
	.4byte	0x5b
	.4byte	0x129
	.uleb128 0xb
	.byte	0
	.uleb128 0x6
	.4byte	0x11e
	.uleb128 0x2
	.4byte	.LASF20
	.byte	0x5
	.byte	0x5f
	.byte	0x13
	.4byte	0x129
	.uleb128 0x7
	.byte	0x1
	.byte	0x2
	.4byte	.LASF21
	.uleb128 0xc
	.4byte	.LASF27
	.byte	0x1
	.byte	0x16
	.byte	0x5
	.4byte	0x31
	.4byte	.LFB99
	.4byte	.LFE99-.LFB99
	.uleb128 0x1
	.byte	0x9c
	.byte	0
	.section	.debug_abbrev,"",%progbits
.Ldebug_abbrev0:
	.uleb128 0x1
	.uleb128 0x11
	.byte	0x1
	.uleb128 0x25
	.uleb128 0xe
	.uleb128 0x13
	.uleb128 0xb
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x1b
	.uleb128 0xe
	.uleb128 0x55
	.uleb128 0x17
	.uleb128 0x11
	.uleb128 0x1
	.uleb128 0x10
	.uleb128 0x17
	.byte	0
	.byte	0
	.uleb128 0x2
	.uleb128 0x34
	.byte	0
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x3f
	.uleb128 0x19
	.uleb128 0x3c
	.uleb128 0x19
	.byte	0
	.byte	0
	.uleb128 0x3
	.uleb128 0x24
	.byte	0
	.uleb128 0xb
	.uleb128 0xb
	.uleb128 0x3e
	.uleb128 0xb
	.uleb128 0x3
	.uleb128 0x8
	.byte	0
	.byte	0
	.uleb128 0x4
	.uleb128 0x1
	.byte	0x1
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x1
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x5
	.uleb128 0x21
	.byte	0
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x2f
	.uleb128 0xb
	.byte	0
	.byte	0
	.uleb128 0x6
	.uleb128 0x26
	.byte	0
	.uleb128 0x49
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x7
	.uleb128 0x24
	.byte	0
	.uleb128 0xb
	.uleb128 0xb
	.uleb128 0x3e
	.uleb128 0xb
	.uleb128 0x3
	.uleb128 0xe
	.byte	0
	.byte	0
	.uleb128 0x8
	.uleb128 0x34
	.byte	0
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x1c
	.uleb128 0x8
	.byte	0
	.byte	0
	.uleb128 0x9
	.uleb128 0xf
	.byte	0
	.uleb128 0xb
	.uleb128 0xb
	.uleb128 0x49
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0xa
	.uleb128 0x16
	.byte	0
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x49
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0xb
	.uleb128 0x21
	.byte	0
	.byte	0
	.byte	0
	.uleb128 0xc
	.uleb128 0x2e
	.byte	0
	.uleb128 0x3f
	.uleb128 0x19
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x27
	.uleb128 0x19
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x11
	.uleb128 0x1
	.uleb128 0x12
	.uleb128 0x6
	.uleb128 0x40
	.uleb128 0x18
	.uleb128 0x2117
	.uleb128 0x19
	.byte	0
	.byte	0
	.byte	0
	.section	.debug_aranges,"",%progbits
	.4byte	0x1c
	.2byte	0x2
	.4byte	.Ldebug_info0
	.byte	0x4
	.byte	0
	.2byte	0
	.2byte	0
	.4byte	.LFB99
	.4byte	.LFE99-.LFB99
	.4byte	0
	.4byte	0
	.section	.debug_ranges,"",%progbits
.Ldebug_ranges0:
	.4byte	.LFB99
	.4byte	.LFE99
	.4byte	0
	.4byte	0
	.section	.debug_line,"",%progbits
.Ldebug_line0:
	.section	.debug_str,"MS",%progbits,1
.LASF15:
	.ascii	"_datarelrolocal_start_ofs\000"
.LASF0:
	.ascii	"unsigned int\000"
.LASF27:
	.ascii	"main\000"
.LASF26:
	.ascii	"ulong\000"
.LASF22:
	.ascii	"GNU C11 9.4.0 -marm -mno-thumb-interwork -mabi=aapc"
	.ascii	"s-linux -mword-relocations -mno-unaligned-access -m"
	.ascii	"float-abi=soft -mgeneral-regs-only -mtune=generic-a"
	.ascii	"rmv7-a -mfpu=vfpv3-d16 -mtls-dialect=gnu -march=arm"
	.ascii	"v7-a -gdwarf-4 -Os -std=gnu11 -fno-builtin -ffreest"
	.ascii	"anding -fshort-wchar -fno-strict-aliasing -fno-stac"
	.ascii	"k-protector -fno-delete-null-pointer-checks -fstack"
	.ascii	"-usage -fno-pic -ffunction-sections -fdata-sections"
	.ascii	" -fno-common -ffixed-r9\000"
.LASF2:
	.ascii	"long unsigned int\000"
.LASF12:
	.ascii	"IRQ_STACK_START\000"
.LASF9:
	.ascii	"long long unsigned int\000"
.LASF16:
	.ascii	"_datarellocal_start_ofs\000"
.LASF11:
	.ascii	"___strtok\000"
.LASF7:
	.ascii	"unsigned char\000"
.LASF1:
	.ascii	"char\000"
.LASF4:
	.ascii	"long int\000"
.LASF25:
	.ascii	"error_message\000"
.LASF21:
	.ascii	"_Bool\000"
.LASF5:
	.ascii	"long long int\000"
.LASF24:
	.ascii	"/home/ny/QEMU/office_linux/u-boot\000"
.LASF13:
	.ascii	"FIQ_STACK_START\000"
.LASF3:
	.ascii	"short unsigned int\000"
.LASF18:
	.ascii	"IRQ_STACK_START_IN\000"
.LASF23:
	.ascii	"arch/arm/lib/asm-offsets.c\000"
.LASF10:
	.ascii	"errno\000"
.LASF19:
	.ascii	"long double\000"
.LASF8:
	.ascii	"short int\000"
.LASF17:
	.ascii	"_datarelro_start_ofs\000"
.LASF14:
	.ascii	"_datarel_start_ofs\000"
.LASF6:
	.ascii	"signed char\000"
.LASF20:
	.ascii	"default_environment\000"
	.ident	"GCC: (Ubuntu 9.4.0-1ubuntu1~20.04.2) 9.4.0"
	.section	.note.GNU-stack,"",%progbits
