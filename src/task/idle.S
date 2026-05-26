.globl idle_loop
idle_loop:
	wfi
	j idle_loop