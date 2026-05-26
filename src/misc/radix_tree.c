#include <misc/radix_tree.h>
#include <misc/string.h>
#include <misc/errno.h>
#include <mm/kalloc.h>

/* 创建新节点 */
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

/* 释放节点及其子节点 */
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

/* 初始化基数树 */
void rxtree_init(struct rxtree *tree) {
	tree->rnode = NULL;
}

/* 插入键值对（递归实现） */
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

/* 插入键值对（接口函数） */
int rxtree_insert(struct rxtree *tree, u64 key, void *value) {
	return rxtree_insert_node(&tree->rnode, key, value, 0);
}

/* 查找键对应的值 */
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

/* 删除键值对（递归实现） */
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

/* 删除键值对（接口函数） */
int rxtree_delete(struct rxtree *tree, u64 key) {
	return rxtree_delete_node(&tree->rnode, key, 0);
}

/* 释放整个基数树 */
void rxtree_free(struct rxtree *tree) {
	rxtree_node_free(tree->rnode);
	tree->rnode = NULL;
}
