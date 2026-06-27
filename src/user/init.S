#
# shell.S — Interactive shell for NoobKernel
# Built-in commands: help, echo, shutdown, exit, clear
# Runs as init process (pid=2)
#

.section .rodata
.equ SYS_read,       63
.equ SYS_write,      64
.equ SYS_openat,     56
.equ SYS_close,      57
.equ SYS_getdents64, 61
.equ SYS_exit,       93
.equ SYS_shutdown,   500
.equ SYS_reboot,     501

prompt:      .string "$ "
newline:     .string "\r\n"
erase_seq:   .byte 0x08, 0x20, 0x08   # \b space \b — erase one char on terminal
clear_seq:   .string "\033[2J\033[H"   # clear screen + home cursor

str_help:    .string "help"
str_echo:    .string "echo"
str_reboot:  .string "reboot"
str_shutdown:.string "shutdown"
str_exit:    .string "exit"
str_clear:   .string "clear"

msg_help:    .string "help       - Show this help message\r\necho <text> - Print the specified text\r\nreboot     - Shutdown and restart\r\nshutdown   - Power off the system\r\nexit       - Exit the shell\r\nclear      - Clear the terminal\r\n"
msg_unknown: .string "Unknown command\r\n"
str_ls:      .string "ls"
str_cat:     .string "cat"
root_path:   .string "/"
msg_cat_usage: .string "Usage: cat <path>\r\n"
msg_cat_fail:  .string "cat: file not found\r\n"


.section .bss
.balign 4
buf:         .space 128               # command line buffer

.section .text
.globl _start
_start:
	li sp, 0x80000000              # USER_TOP

	#
	# Main shell loop
	#
shell_loop:
	# Print "$ " prompt
	li a7, SYS_write
	li a0, 1
	la a1, prompt
	li a2, 2
	ecall

	la s1, buf                     # s1 = buffer base
	li s0, 0                       # s0 = buffer index

	#
	# Read one character at a time
	#
read_ch:
	li a7, SYS_read
	li a0, 0                       # stdin
	add a1, s1, s0                 # &buf[index]
	li a2, 1                       # 1 byte
	ecall

	add t0, s1, s0
	lb t1, 0(t0)                   # t1 = character

	# Check for newline (\n = 0x0a)
	li t2, 10
	beq t1, t2, handle_enter
		li t2, 13
		beq t1, t2, handle_enter

	# Check for backspace (DEL = 0x7f or BS = 0x08)
	li t2, 0x7f
	beq t1, t2, handle_backspace
	li t2, 0x08
	beq t1, t2, handle_backspace

	# Regular character: store then echo
	# (character is already at buf[index] from sys_read)
	li a7, SYS_write                       # SYS_write
	li a0, 1                       # stdout
	add a1, s1, s0                 # &buf[index]
	li a2, 1
	ecall

	addi s0, s0, 1                 # index++

	# Clamp to 127 max
	li t2, 127
	ble s0, t2, read_ch
	li s0, 127
	j read_ch

	#
	# Backspace: remove last character
	#
handle_backspace:
	beqz s0, read_ch               # buffer empty, ignore

	addi s0, s0, -1                # index--

	# Print erase sequence: \b space \b
	li a7, SYS_write
	li a0, 1
	la a1, erase_seq
	li a2, 3
	ecall
	j read_ch

	#
	# Enter: null-terminate, parse, and execute command
	#
handle_enter:
	# Echo newline
	li a7, SYS_write
	li a0, 1
	la a1, newline
	li a2, 2
	ecall

	# Null-terminate buffer at index s0 (offset where newline was)
	add t0, s1, s0
	sb zero, 0(t0)                 # buf[index] = '\0'

	# If empty line, go back to prompt
	lb t0, 0(s1)
	beqz t0, shell_loop

	# ---- strcmp(buf, "help") ----
	la a0, str_help
	mv a1, s1
	jal strcmp
	beqz a0, cmd_help

	# ---- strcmp(buf, "clear") ----
	la a0, str_clear
	mv a1, s1
	jal strcmp
	beqz a0, cmd_clear

	# ---- strcmp(buf, "exit") ----
	la a0, str_exit
	mv a1, s1
	jal strcmp
	beqz a0, cmd_exit

	# ---- strcmp(buf, "reboot") ----
	la a0, str_reboot
	mv a1, s1
	jal strcmp
	beqz a0, cmd_reboot_handler

	# ---- strcmp(buf, "shutdown") ----
	la a0, str_shutdown
	mv a1, s1
	jal strcmp
	beqz a0, cmd_shutdown_handler

	# ---- strcmp(buf, "ls") ----
	la a0, str_ls
	mv a1, s1
	jal strcmp
	beqz a0, cmd_ls

	# ---- strcmp(buf, "cat") ----
	la a0, str_cat
	mv a1, s1
	li a2, 3
	jal strncmp
	beqz a0, cmd_cat

	# ---- echo check: first 4 chars == "echo" ----
	la a0, str_echo                # "echo"
	mv a1, s1
	li a2, 4
	jal strncmp
	beqz a0, cmd_echo

	# ---- Unknown command ----
cmd_unknown_handler:
	la a1, msg_unknown
	jal puts
	j shell_loop

	#
	# Command handlers
	#
cmd_help:
	la a1, msg_help
	jal puts
	j shell_loop


cmd_clear:
	la a1, clear_seq
	mv t0, a1
	li t1, 0
1:	lb t2, 0(t0)
	beqz t2, 2f
	addi t1, t1, 1
	addi t0, t0, 1
	j 1b
2:	mv a2, t1
	li a7, SYS_write
	li a0, 1
	ecall
	j shell_loop


	#
	# ls: list root directory
	#
cmd_ls:
	/* Open "/" */
	li a7, SYS_openat			# SYS_open
	la a0, root_path		# "/"
	li a1, 0			# O_RDONLY
	ecall
	bltz a0, ls_fail		# skip if error
	mv s2, a0			# s2 = fd

	addi sp, sp, -280		# dirent buffer

	ls_loop:
		li a7, SYS_getdents64			# SYS_getdents
		mv a0, s2
		mv a1, sp
		li a2, 280
		ecall
		blez a0, ls_done

		/* Compute name length (start at offset 20) */
		addi t0, sp, 19			# &dirent.d_name
		mv t1, t0			# running pointer
	1:	lb t2, 0(t1)
		beqz t2, 2f
		addi t1, t1, 1
		j 1b
	2:	sub a2, t1, t0			# name length

		/* Print name */
		li a7, SYS_write
		li a0, 1
		mv a1, t0
		ecall

		/* Print newline */
		li a7, SYS_write
		li a0, 1
		la a1, newline
		li a2, 2
		ecall
		j ls_loop

	ls_done:
		addi sp, sp, 280
		/* Close */
		li a7, SYS_close			# SYS_close
		mv a0, s2
		ecall
		j shell_loop
	ls_fail:
		la a1, msg_cat_fail
		jal puts
		j shell_loop

	#
	# cat: read and print a file
	#
cmd_cat:
	add t0, s1, zero
	lb t1, 4(t0)			# buf[4]
	beqz t1, cat_usage
	li t2, 0x20			# ' '
	bne t1, t2, cat_usage

	/* Open file at buf+5 */
	li a7, SYS_openat			# SYS_open
	addi a0, s1, 4			# path starts at buf[4]
	li a1, 0			# O_RDONLY
	ecall
	bltz a0, cat_fail
	mv s2, a0			# s2 = fd

	addi sp, sp, -512		# read buffer

cat_loop:
	li a7, SYS_read			# SYS_read
	mv a0, s2
	mv a1, sp
	li a2, 512
	ecall
	blez a0, cat_done
	mv s3, a0			# s3 = bytes read

	li a7, SYS_write			# SYS_write
	li a0, 1
	mv a1, sp
	mv a2, s3
	ecall
	j cat_loop

cat_done:
	addi sp, sp, 512
	li a7, SYS_close			# SYS_close
	mv a0, s2
	ecall
	j shell_loop

cat_usage:	
	la a1, msg_cat_usage
	jal puts
	j shell_loop

cat_fail:	
	la a1, msg_cat_fail
	jal puts
	j shell_loop

cmd_exit:
	li a7, SYS_exit
	li a0, 0                       # status = 0
	ecall
	# Unreachable — fall through to shutdown
	j shell_loop

cmd_reboot_handler:
		li a7, SYS_reboot
		ecall
		# Unreachable
		j shell_loop

cmd_shutdown_handler:
	li a7, SYS_shutdown
	ecall
	# Unreachable
	j shell_loop

	#
	# echo: print remaining text after "echo "
	# After strncmp match on "echo":
	#   buf[4] should be '\0' (no arg) or ' ' (has arg)
	#
cmd_echo:
	add t0, s1, zero
	lb t1, 3(t0)                   # buf[3] — "cat" 之后的空格位置
	beqz t1, echo_no_arg
	li t2, 0x20                    # ' '
	bne t1, t2, cmd_unknown_handler

	# Print buf+5 and trailing newline
	addi a1, s1, 5
	jal puts
	li a7, SYS_write
	li a0, 1
	la a1, newline
	li a2, 2
	ecall
	j shell_loop

echo_no_arg:
	# Just print newline (already echoed above)
	j shell_loop

	#
	# puts: print null-terminated string
	# a1 = string pointer
	#
puts:
	mv t0, a1                      # save start
	mv t1, a1                      # running pointer
1:	lb t2, 0(t1)
	beqz t2, 2f
	addi t1, t1, 1
	j 1b
2:	sub a2, t1, t0                 # length = end - start
	li a7, SYS_write
	li a0, 1
	mv a1, t0
	ecall
	ret

	#
	# strcmp: compare two null-terminated strings
	# a0 = s1, a1 = s2
	# returns a0 = 0 if equal, non-zero if not
	#
strcmp:
	mv t0, a0
	mv t1, a1
1:	lb t2, 0(t0)
	lb t3, 0(t1)
	bne t2, t3, 2f                 # different → return 1
	beqz t2, 3f                    # both end → return 0
	addi t0, t0, 1
	addi t1, t1, 1
	j 1b
2:	li a0, 1
	ret
3:	li a0, 0
	ret

	#
	# strncmp: compare first n chars
	# a0 = s1, a1 = s2, a2 = n
	# returns a0 = 0 if match, 1 if not
	#
strncmp:
	beqz a2, strncmp_eq            # n=0 → equal
	mv t0, a0
	mv t1, a1
	mv t2, a2
	li t4, 0                       # index
1:	add t5, t0, t4
	add t6, t1, t4
	lb t3, 0(t5)
	lb t5, 0(t6)
	bne t3, t5, 2f                 # different
	addi t4, t4, 1
	blt t4, t2, 1b                 # more to compare?
strncmp_eq:
	li a0, 0
	ret
2:	li a0, 1
	ret
