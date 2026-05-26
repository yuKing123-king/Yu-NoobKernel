#pragma once

#include <misc/stdint.h>

void plic_init(void);

// 设置指定中断源的优先级，数值越小优先级越高
void plic_set_priority(u32 irqno, u32 priority);

// 为指定Hart的特定模式使能（开启）指定中断
void plic_enable(char mode, u64 hartid, u32 irqno);

// 为指定Hart的特定模式禁用（关闭）指定中断
void plic_disable(char mode, u64 hartid, u32 irqno);

// 设置指定Hart特定模式下的中断屏蔽阈值，小于等于该阈值的中断将被屏蔽
void plic_set_threshold(char mode, u64 hartid, u32 threshold);

// 申领并获取指定Hart特定模式下当前挂起的最高优先级中断 ID
u32 plic_claim(char mode, u64 hartid);

// 通知PLIC指定Hart特定模式下对该中断 ID 的处理已完成
void plic_complete(char mode, u64 hartid, u32 irqno);
