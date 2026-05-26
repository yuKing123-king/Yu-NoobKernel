#pragma once

#include <misc/stddef.h>
#include <misc/stdbool.h>
#include <misc/log.h>

#define RADIX_TREE_MAP_SHIFT 2
#define RADIX_TREE_MAP_SIZE (1UL << RADIX_TREE_MAP_SHIFT)

/* 基数树节点结构 */
struct rxtree_node {
	u64 prefix; /* 当前节点的前缀 */
	bool is_leaf; /* 是否为叶子节点 */
	void *value; /* 叶子节点的值 */
	struct rxtree_node *children[RADIX_TREE_MAP_SIZE]; /* 子节点数组 */
};

/* 基数树根结构 */
struct rxtree {
	struct rxtree_node *rnode; /* 根节点 */
};

/* 初始化基数树 */
void rxtree_init(struct rxtree *tree);

/* 插入键值对 */
int rxtree_insert(struct rxtree *tree, u64 key, void *value);

/* 查找键对应的值 */
void *rxtree_lookup(struct rxtree *tree, u64 key);

/* 删除键值对 */
int rxtree_delete(struct rxtree *tree, u64 key);

/* 释放整个基数树 */
void rxtree_free(struct rxtree *tree);

