#
# 空闲线程主循环：执行wfi指令等待中断，唤醒后继续循环
#
.globl idle_loop
idle_loop:
	wfi
	j idle_loop