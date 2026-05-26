#pragma once

#include <misc/stddef.h>
#include <misc/list.h>

struct hash_node {
	struct list_head list; // 链表节点
	void *key;	       // 键（用户传入）
	void *value;	       // 值（用户传入）
};

struct hashtable {
	struct list_head *buckets; // 桶数组
	size_t size;		   // 桶数量（2^n）
	size_t count;		   // 元素数量
};

// 哈希函数（FNV-1a，简单高效）
static inline u32 hash_ptr(void *key, size_t size)
{
	u32 hash = 0x811C9DC5;
	uintptr_t k = (uintptr_t)key;
	for (int i = 0; i < sizeof(uintptr_t); i++) {
		hash ^= (k & 0xFF);
		hash *= 0x01000193;
		k >>= 8;
	}
	return hash & (size - 1); // size 必须是 2^n
}

// 初始化哈希表
int hashtable_init(struct hashtable *ht, size_t size);

// 插入/更新
int hashtable_insert(struct hashtable *ht, void *key, void *value);

// 查找
void *hashtable_lookup(struct hashtable *ht, void *key);

// 删除
void *hashtable_delete(struct hashtable *ht, void *key);

// 遍历（需用户加锁）
#define hashtable_foreach(ht, node)                                            \
	for (size_t i = 0; i < (ht)->size; i++)                                \
	list_for_each_entry(node, &(ht)->buckets[i], list)

// 销毁（不释放 value）
void hashtable_destroy(struct hashtable *ht);
