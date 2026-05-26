#pragma once

#include <misc/stddef.h>

struct list_head {
	struct list_head *next, *prev;
};

// 初始化链表头
static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

// 判断链表是否为空
static inline int list_empty(const struct list_head *head)
{
	return head->next == head;
}

// 将新节点插入到 prev 和 next 之间
static inline void __list_add(struct list_head *new, struct list_head *prev,
			      struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

// 在链表头插入节点
static inline void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);
}

// 在链表尾插入节点
static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
	__list_add(new, head->prev, head);
}

// 删除节点
static inline void __list_del(struct list_head *prev, struct list_head *next)
{
	next->prev = prev;
	prev->next = next;
}

// 从链表中删除一个节点
static inline void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
}

static inline void list_move(struct list_head *new, struct list_head *head)
{
	list_del(new);
	list_add(new, head);
}

// 获取包含链表节点的结构体指针
#define list_entry(ptr, type, member)                                          \
	((type *)((char *)(ptr) - offsetof(type, member)))

// 遍历链表（安全用于读取）
#define list_for_each(pos, head)                                               \
	for (pos = (head)->next; pos != (head); pos = pos->next)

// 获取链表的第一个元素
#define list_first_entry(head, type, member)                                   \
	list_entry((head)->next, type, member)

// 安全遍历（删除时可用）
#define list_for_each_safe(pos, n, head)                                       \
	for (pos = (head)->next, n = pos->next; pos != (head);                 \
	     pos = n, n = pos->next)

// 遍历链表，直接获取结构体指针
#define list_for_each_entry(pos, head, member)                                 \
	for (pos = list_entry((head)->next, typeof(*pos), member);             \
	     &pos->member != (head);                                           \
	     pos = list_entry(pos->member.next, typeof(*pos), member))

// 安全遍历链表（可在循环中删除节点）
#define list_for_each_entry_safe(pos, n, head, member)                         \
	for (pos = list_entry((head)->next, typeof(*pos), member),             \
	    n = list_entry(pos->member.next, typeof(*pos), member);            \
	     &pos->member != (head);                                           \
	     pos = n, n = list_entry(n->member.next, typeof(*n), member))

#define list_for_each_entry_reverse(pos, head, member)                         \
	for (pos = list_entry((head)->prev, typeof(*pos), member);             \
	     &pos->member != (head);                                           \
	     pos = list_entry(pos->member.prev, typeof(*pos), member))
