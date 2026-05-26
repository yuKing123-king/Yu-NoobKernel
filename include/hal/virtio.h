/**
 * @file virtio.h
  VirtIO 设备定义
 *
 * 该头文件定义了 VirtIO 设备的 MMIO 接口和描述符结构。
 * 目前仅在 QEMU 环境中测试过。
 *
 * VirtIO 规范参考：
 * https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf
 */

#pragma once

#include <misc/stdint.h>
#include <misc/bit.h>
#include <misc/endian.h>
#include <sync/spinlock.h>
#include <config.h>

static inline u32 mmio_read32(volatile void *addr)
{
	u32 val;
	__asm__ __volatile__("lw %0, 0(%1)" : "=r"(val) : "r"(addr) : "memory");
	return val;
}

static inline void mmio_write32(volatile void *addr, u32 val)
{
	__asm__ __volatile__("sw %0, 0(%1)" : : "r"(val), "r"(addr) : "memory");
}

/**
  VirtIO MMIO 寄存器结构体
 *
 * VirtIO 设备的 MMIO 寄存器布局。
 * 所有寄存器均为 32 位宽度。
 */
struct virtio_mmio_regs {
	volatile le32 magic_value;     /* (R) 魔数值，必须为 0x74726976 */
	volatile le32 version;	       /* (R) 版本号，应为 2 */
	volatile le32 device_id;       /* (R) 设备类型：1 为网络，2 为磁盘 */
	volatile le32 vendor_id;       /* (R) 厂商 ID：0x554d4551 */
	volatile le32 device_features; /* (R) 设备特性位图 */
	volatile le32 device_features_sel; /* (W) 设备特性位图范围选择 */
	u8 _reserved1[8];
	volatile le32 driver_features;	   /* (W) 驱动程序特性位图 */
	volatile le32 driver_features_sel; /* (W) 驱动程序特性位图范围选择 */
	u8 _reserved2[8];
	volatile le32 queue_sel;     /* (W) 队列选择寄存器 */
	volatile le32 queue_num_max; /* (R) 当前队列最大大小 */
	volatile le32 queue_num;     /* (W) 当前队列大小 */
	u8 _reserved3[8];
	volatile le32 queue_ready; /* (RW) 队列准备状态 */
	u8 _reserved4[8];
	volatile le32 queue_notify; /* (W) 队列通知寄存器 */
	u8 _reserved5[12];
	volatile le32 interrupt_status; /* (R) 中断状态寄存器 */
	volatile le32 interrupt_ack;	/* (W) 中断确认寄存器 */
	u8 _reserved6[8];
	volatile le32 status; /* (RW) 设备状态寄存器 */
	u8 _reserved7[12];
	volatile le64 queue_desc; /* (W) 队列描述符表物理地址 */
	u8 _reserved8[8];
	volatile le64
	    queue_driver_desc; /* (W) 驱动程序描述符物理地址（只写） */
	u8 _reserved9[8];
	volatile le64 queue_device_desc; /* (W) 设备描述符物理地址（只写） */
	u8 _reserved10[84];
	volatile le32 config_generation; /* (R) 配置空间摘要 */
	volatile u8 config[];		 /* (RW) 配置空间 */
} __attribute__((__packed__));

#define VIRTIO_DEVICE_TYPE_NET 1
#define VIRTIO_DEVICE_TYPE_DISK 2

/* 状态寄存器位定义 */
#define VIRTIO_CONFIG_S_ACKNOWLEDGE BIT(0)	  /* 设备已确认 */
#define VIRTIO_CONFIG_S_DRIVER BIT(1)		  /* 驱动程序已加载 */
#define VIRTIO_CONFIG_S_DRIVER_OK BIT(2)	  /* 驱动程序就绪 */
#define VIRTIO_CONFIG_S_FEATURES_OK BIT(3)	  /* 特性协商完成 */
#define VIRTIO_CONFIG_S_DEVICE_NEEDS_RESET BIT(6) /* 错误无法恢复，需要重启 */
#define VIRTIO_CONFIG_S_FAILED BIT(7)		  /* 设备出现致命错误 */
/* 设备特性位定义 */
#define VIRTIO_BLK_F_RO BIT(5)		    /* 磁盘只读 */
#define VIRTIO_BLK_F_SCSI BIT(7)	    /* 支持 SCSI 命令透传 */
#define VIRTIO_BLK_F_CONFIG_WCE BIT(11)	    /* 配置中支持回写模式 */
#define VIRTIO_BLK_F_MQ BIT(12)		    /* 支持多个虚拟队列 */
#define VIRTIO_F_ANY_LAYOUT BIT(27)	    /* 支持任意描述符布局 */
#define VIRTIO_RING_F_INDIRECT_DESC BIT(28) /* 支持间接描述符 */
#define VIRTIO_RING_F_EVENT_IDX BIT(29)	    /* 支持事件索引 */

/* 单个描述符结构体 */
struct virtq_desc {
	u64 addr;  /* 物理地址 */
	le32 len;  /* 数据长度 */
	u16 flags; /* 标志位 */
	u16 next;  /* 下一个描述符索引（如果 VRING_DESC_F_NEXT 置位） */
};
#define VRING_DESC_F_NEXT 1  /* 链接到另一个描述符 */
#define VRING_DESC_F_WRITE 2 /* 设备写操作（vs 读） */

/* 可用环结构体 */
struct virtq_avail {
	u16 flags;  /* 标志位（始终为零） */
	u16 idx;    /* 驱动程序下一个写入的 ring 索引 */
	u16 ring[]; /* 链头描述符编号 */
};

/* 已用环条目结构体
 * 设备通过此结构体通知驱动程序已完成请求。
 */
struct virtq_used_elem {
	le32 id;  /* 已完成描述符链的起始索引 */
	le32 len; /* 已处理数据的长度 */
};

/* 已用环结构体 */
struct virtq_used {
	u16 flags;		       /* 标志位（始终为零） */
	u16 idx;		       /* 设备添加 ring[] 条目时递增 */
	struct virtq_used_elem ring[]; /* 已用环条目数组 */
};

#define VIRTIO_BLK_T_IN 0  /* 读取磁盘 */
#define VIRTIO_BLK_T_OUT 1 /* 写入磁盘 */

/* 块设备请求结构体
 * 磁盘请求中第一个描述符的格式。
 * 后跟两个包含数据块和一个状态字节的描述符。
 */
struct virtio_blk_req {
	le32 type;     /* 请求类型：VIRTIO_BLK_T_IN 或 VIRTIO_BLK_T_OUT */
	le32 reserved; /* 保留字段 */
	u64 sector;    /* 扇区号 */
};

#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_S_IOERR 1
#define VIRTIO_BLK_S_UNSUPP 2

struct virtio_blk_config {
	u64 capacity;
	u32 size_max;
	u32 seg_max;
	u16 geometry_cylinders;
	u8 geometry_heads;
	u8 geometry_sectors;
	u32 block_size;
	u8 physical_block_exp;
	u8 alignment_offset;
	u16 min_io_size;
	u32 opt_io_size;
	u8 writeback;
	u8 unused;
};

#define VRING_ALIGN 4096

static inline size_t vring_size(u16 num)
{
	size_t desc_size = sizeof(struct virtq_desc) * num;
	size_t avail_size = sizeof(u16) * (3 + num);
	size_t used_size =
	    sizeof(u16) * 3 + sizeof(struct virtq_used_elem) * num;
	return desc_size + avail_size + used_size + VRING_ALIGN * 2;
}

struct virtq_buf {
	void *addr;
	u32 len;
};

struct virtq {
	struct virtio_mmio_regs *regs;
	u16 queue_idx;
	struct virtq_desc *descs;
	struct virtq_avail *avail;
	struct virtq_used *used;
	u16 num;
	u16 free_head;
	u16 num_free;
	u16 avail_idx;
	u16 used_idx;
	spinlock_t lock;
	void *data[VIRTQ_SIZE];
};

struct virtq *virtq_create(struct virtio_mmio_regs *regs, u16 queue_idx,
			   u16 num);
void virtq_destroy(struct virtq *vq);
int virtq_add_buf(struct virtq *vq, struct virtq_buf *in_bufs, int n_in,
		  struct virtq_buf *out_bufs, int n_out, void *token);
void *virtq_get_buf(struct virtq *vq, u32 *len);
bool virtq_has_buf(struct virtq *vq);
void virtq_kick(struct virtq *vq);
u16 virtq_desc_count(struct virtq *vq);
