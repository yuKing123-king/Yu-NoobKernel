#include <misc/hashtable.h>
#include <mm/kalloc.h>
#include <misc/string.h>
#include <misc/errno.h>

/*
 * 初始化哈希表
 * @param ht: 哈希表对象指针
 * @param size: 桶数量（必须是 2 的幂）
 * @return: 成功返回 0，size 无效返回 -EINVAL，内存分配失败返回 -ENOMEM
 */
int hashtable_init(struct hashtable *ht, size_t size)
{
	// 确保 size 是 2 的幂
	if (size == 0 || (size & (size - 1)) != 0)
		return -EINVAL;

	ht->buckets = kcalloc(size, sizeof(struct list_head));
	if (!ht->buckets)
		return -ENOMEM;

	for (size_t i = 0; i < size; i++)
		INIT_LIST_HEAD(&ht->buckets[i]);

	ht->size = size;
	ht->count = 0;
	return 0;
}

/*
 * 向哈希表中插入键值对（若 key 已存在则更新 value）
 * @param ht: 哈希表对象指针
 * @param key: 键指针
 * @param value: 值指针
 * @return: 成功返回 0，内存分配失败返回 -ENOMEM
 */
int hashtable_insert(struct hashtable *ht, void *key, void *value)
{
	u32 idx = hash_ptr(key, ht->size);
	struct hash_node *node;

	// 先查找是否已存在
	list_for_each_entry(node, &ht->buckets[idx], list)
	{
		if (node->key == key) {
			node->value = value;
			return 0; // 更新
		}
	}

	// 新建节点
	node = kmalloc(sizeof(struct hash_node));
	if (!node)
		return -ENOMEM;

	node->key = key;
	node->value = value;
	list_add(&node->list, &ht->buckets[idx]);
	ht->count++;
	return 0;
}

/*
 * 在哈希表中查找指定键对应的值
 * @param ht: 哈希表对象指针
 * @param key: 要查找的键指针
 * @return: 找到返回值指针，未找到返回 NULL
 */
void *hashtable_lookup(struct hashtable *ht, void *key)
{
	u32 idx = hash_ptr(key, ht->size);
	struct hash_node *node;

	list_for_each_entry(node, &ht->buckets[idx], list)
	{
		if (node->key == key)
			return node->value;
	}
	return NULL;
}

/*
 * 从哈希表中删除指定键的键值对
 * @param ht: 哈希表对象指针
 * @param key: 要删除的键指针
 * @return: 被删除的值指针，键不存在返回 NULL
 */
void *hashtable_delete(struct hashtable *ht, void *key)
{
	u32 idx = hash_ptr(key, ht->size);
	struct hash_node *node;

	list_for_each_entry(node, &ht->buckets[idx], list)
	{
		if (node->key == key) {
			void *value = node->value;
			list_del(&node->list);
			kfree(node);
			ht->count--;
			return value;
		}
	}
	return NULL;
}

/*
 * 销毁哈希表，释放所有节点和桶数组内存
 * @param ht: 哈希表对象指针
 */
void hashtable_destroy(struct hashtable *ht)
{
	struct hash_node *node, *tmp;
	for (size_t i = 0; i < ht->size; i++) {
		list_for_each_entry_safe(node, tmp, &ht->buckets[i], list)
		{
			list_del(&node->list);
			kfree(node);
		}
	}
	kfree(ht->buckets);
	ht->buckets = NULL;
	ht->size = 0;
	ht->count = 0;
}
