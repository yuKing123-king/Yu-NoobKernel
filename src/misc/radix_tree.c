#include <misc/radix_tree.h>
#include <misc/string.h>
#include <misc/errno.h>
#include <mm/kalloc.h>

/*
 * 创建基数树新节点
 * @param prefix: 节点前缀键值
 * @param is_leaf: 是否为叶子节点
 * @param value: 叶子节点存储的值指针
 * @return: 新节点指针，内存分配失败返回 NULL
 */
static struct rxtree_node *rxtree_node_alloc(u64 prefix, bool is_leaf,
					     void *value) {
	struct rxtree_node *node = kmalloc(sizeof(*node));
	if (!node)
		return NULL;

	node->prefix = prefix;
	node->is_leaf = is_leaf;
	node->value = value;
	memset(node->children, 0, sizeof(node->children));

	return node;
}

/*
 * 递归释放基数树节点及其所有子节点
 * @param node: 待释放的节点指针（可为 NULL）
 */
static void rxtree_node_free(struct rxtree_node *node) {
	if (!node)
		return;

	for (int i = 0; i < RADIX_TREE_MAP_SIZE; i++) {
		if (node->children[i]) {
			rxtree_node_free(node->children[i]);
			node->children[i] = NULL;
		}
	}

	kfree(node);
}

/*
 * 初始化基数树
 * @param tree: 基数树对象指针
 */
void rxtree_init(struct rxtree *tree) {
	tree->rnode = NULL;
}

/*
 * 递归向基数树中插入键值对，必要时分裂叶子节点
 * @param node: 当前节点指针的二级指针
 * @param key: 键值
 * @param value: 值指针
 * @param depth: 当前递归深度
 * @return: 成功返回 0，内存分配失败返回 -ENOMEM
 */
static int rxtree_insert_node(struct rxtree_node **node, u64 key, void *value,
			      int depth) {
	if (!*node) {
		*node = rxtree_node_alloc(key, true, value);
		return *node ? 0 : -ENOMEM;
	}

	struct rxtree_node *current = *node;

	if (current->is_leaf) {
		if (current->prefix == key) {
			current->value = value;
			return 0;
		}

		/* 分裂叶子节点 */
		struct rxtree_node *parent = rxtree_node_alloc(0, false, NULL);
		if (!parent)
			return -ENOMEM;

		int shift = 64 - (depth + 1) * RADIX_TREE_MAP_SHIFT;
		u64 mask = ((u64)RADIX_TREE_MAP_SIZE - 1) << shift;
		u64 idx1 = (key & mask) >> shift;
		u64 idx2 = (current->prefix & mask) >> shift;

		if (idx1 == idx2) {
			if (rxtree_insert_node(&parent->children[idx1], key,
					       value, depth + 1) ||
			    rxtree_insert_node(&parent->children[idx2],
					       current->prefix, current->value,
					       depth + 1)) {
				rxtree_node_free(parent);
				return -ENOMEM;
			}
			memcpy(current->children, parent->children,
			       sizeof(current->children));
			current->is_leaf = false;
			current->value = NULL;
			kfree(parent);
		} else {
			parent->children[idx1] =
				rxtree_node_alloc(key, true, value);
			parent->children[idx2] = rxtree_node_alloc(
				current->prefix, true, current->value);
			if (!parent->children[idx1] ||
			    !parent->children[idx2]) {
				rxtree_node_free(parent);
				return -ENOMEM;
			}
			*node = parent;
		}
		return 0;
	}

	/* 非叶子节点，继续递归插入 */
	int shift = 64 - (depth + 1) * RADIX_TREE_MAP_SHIFT;
	u64 mask = ((u64)RADIX_TREE_MAP_SIZE - 1) << shift;
	u64 index = (key & mask) >> shift;

	return rxtree_insert_node(&current->children[index], key, value,
				  depth + 1);
}

/*
 * 向基数树中插入键值对（对外接口）
 * @param tree: 基数树对象指针
 * @param key: 键值
 * @param value: 值指针
 * @return: 成功返回 0，失败返回负值错误码
 */
int rxtree_insert(struct rxtree *tree, u64 key, void *value) {
	return rxtree_insert_node(&tree->rnode, key, value, 0);
}

/*
 * 在基数树中查找指定键对应的值
 * @param tree: 基数树对象指针
 * @param key: 要查找的键值
 * @return: 找到返回值指针，未找到返回 NULL
 */
void *rxtree_lookup(struct rxtree *tree, u64 key) {
	struct rxtree_node *node = tree->rnode;
	int depth = 0;

	while (node && !node->is_leaf) {
		int shift = 64 - (depth + 1) * RADIX_TREE_MAP_SHIFT;
		u64 mask = ((u64)RADIX_TREE_MAP_SIZE - 1) << shift;
		u64 index = (key & mask) >> shift;
		node = node->children[index];
		depth++;
	}

	if (node && node->is_leaf && node->prefix == key)
		return node->value;

	return NULL;
}

/*
 * 递归从基数树中删除指定键的键值对，必要时合并节点
 * @param node: 当前节点指针的二级指针
 * @param key: 要删除的键值
 * @param depth: 当前递归深度
 * @return: 成功返回 0，键不存在返回 -EINVAL
 */
static int rxtree_delete_node(struct rxtree_node **node, u64 key, int depth) {
	if (!*node)
		return -EINVAL;

	struct rxtree_node *current = *node;

	if (current->is_leaf) {
		if (current->prefix == key) {
			kfree(current);
			*node = NULL;
			return 0;
		}
		return -EINVAL;
	}

	int shift = 64 - (depth + 1) * RADIX_TREE_MAP_SHIFT;
	u64 mask = ((u64)RADIX_TREE_MAP_SIZE - 1) << shift;
	u64 index = (key & mask) >> shift;

	int ret = rxtree_delete_node(&current->children[index], key, depth + 1);

	if (ret == 0) {
		int count = 0;
		struct rxtree_node *child = NULL;

		for (int i = 0; i < RADIX_TREE_MAP_SIZE; i++) {
			if (current->children[i]) {
				count++;
				child = current->children[i];
			}
		}

		if (count == 1 && child && child->is_leaf) {
			*node = child;
			kfree(current);
		} else if (count == 0) {
			kfree(current);
			*node = NULL;
		}
	}

	return ret;
}

/*
 * 从基数树中删除指定键的键值对（对外接口）
 * @param tree: 基数树对象指针
 * @param key: 要删除的键值
 * @return: 成功返回 0，失败返回负值错误码
 */
int rxtree_delete(struct rxtree *tree, u64 key) {
	return rxtree_delete_node(&tree->rnode, key, 0);
}

/*
 * 释放整个基数树的所有节点内存
 * @param tree: 基数树对象指针
 */
void rxtree_free(struct rxtree *tree) {
	rxtree_node_free(tree->rnode);
	tree->rnode = NULL;
}
