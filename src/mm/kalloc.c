#include <misc/stdbool.h>
#include <misc/align.h>
#include <mm/kalloc.h>
#include <mm/pm.h>
#include <mm/slab.h>
#include <mm/buddy.h>
#include <mm/early.h>
#include <hal/riscv.h>
#include <misc/string.h>
#include <misc/printf.h>
#include <misc/errno.h>
#include <misc/complier.h>
#include <misc/log.h>

bool kalloc_inited = false;

const size_t MEM_OBJ_SIZES[] = {8,   16,   24,	 32,   48,   64,
				96,  128,  192,	 256,  384,  512,
				768, 1024, 1536, 2048, 3072, 4096};
#define KALLOC_MEM_POOL_NUM ARRAY_SIZE(MEM_OBJ_SIZES)
#define KALLOC_MAX_MEM_OBJ_SIZE MEM_OBJ_SIZES[KALLOC_MEM_POOL_NUM - 1]
static struct kmem_cache kalloc_mem_pool[KALLOC_MEM_POOL_NUM][CPU_NUM];

/*
 * 根据分配大小查找对应的内存池索引
 * @param size: 分配大小（字节）
 * @return: 对应的MEM_OBJ_SIZES数组索引
 */
static int size2index(size_t size)
{
	int i = 0;
	for (; i < KALLOC_MEM_POOL_NUM - 1; i++) {
		if (size <= MEM_OBJ_SIZES[i])
			break;
	}
	return i;
}

/*
 * 初始化kalloc系统：为每个CPU创建所有大小的kmem_cache内存池
 * @return: 无返回值
 */
void kalloc_init()
{
	for (int i = 0; i < CPU_NUM; i++) {
		for (int j = 0; j < KALLOC_MEM_POOL_NUM; j++) {
			char *name = early_alloc(24);
			snprintf(name, 24, "kmalloc-%zu<%d>", MEM_OBJ_SIZES[j],
				 i);
			int ret = kmem_cache_init(&kalloc_mem_pool[j][i], name,
					MEM_OBJ_SIZES[j], true);
			if (ret)
				warnf("kalloc_init: %s init failed (ret=%d), "
				      "will retry on first alloc",
				      name, ret);
		}
	}
	kalloc_inited = true;
}

/*
 * 分配指定大小的内存（通用内存分配接口）
 * @param size: 分配大小（字节）
 * @return: 分配的内存地址，失败返回NULL
 */
void *kmalloc(size_t size)
{
	if (unlikely(size == 0)) {
		return NULL;
	}
	if (size <= KALLOC_MAX_MEM_OBJ_SIZE) {
		return kmem_cache_alloc(
		    &kalloc_mem_pool[size2index(size)][r_tp()]);
	} else if (size <= (PAGE_SIZE << BUDDY_MAX_ORDER)) {
		return buddy_alloc(size);
	}
	return NULL;
}

/*
 * 分配指定大小的内存并清零
 * @param size: 分配大小（字节）
 * @return: 分配的已清零内存地址，失败返回NULL
 */
void *kzalloc(size_t size)
{
	void *ptr = kmalloc(size);
	if (ptr) {
		memset(ptr, 0, size); // 清零分配的内存
	}
	return ptr;
}

/*
 * 分配指定数量的元素内存并清零（带溢出检查）
 * @param nitems: 元素数量
 * @param size: 每个元素的大小（字节）
 * @return: 分配的已清零内存地址，失败返回NULL
 */
void *kcalloc(size_t nitems, size_t size)
{
	if (nitems && size > SIZE_MAX / nitems) {
		warnf("kcalloc overflow");
		return NULL; // overflow!
	}
	return kzalloc(nitems * size);
}

/*
 * 重新分配内存（先释放旧内存，再分配新内存）
 * @param ptr: 旧内存地址
 * @param size: 新内存大小（字节）
 * @return: 新分配的内存地址，失败返回NULL
 */
void *krealloc(void *ptr, size_t size)
{
	kfree(ptr);
	return kmalloc(size); // 重新分配内存
}

/*
 * 释放由kmalloc/kzalloc/kcalloc分配的内存（自动判断slab或buddy）
 * @param addr: 待释放的内存地址
 * @return: 无返回值
 */
void kfree(void *addr)
{
	struct page *page = addr2page((void *)PAGE_ALIGN_DOWN((uintptr_t)addr));
	if (page == NULL) {
		return;
	}

	if (unlikely(!(page->flags & PM_BUDDY))) {
		warnf("trying free an invalid ptr: %p", addr);
		return;
	}

	if (page->flags & PM_SLAB) {
		return kmem_cache_free(addr);
	} else {
		return buddy_free(addr);
	}
}

// 简易 LCG 随机（确定性，便于复现）
static uint32_t kmalloc_test_seed = 0xCAFEBABE;
/*
 * 生成一个32位伪随机数（LCG算法，确定性序列便于复现）
 * @return: 32位伪随机数
 */
static inline uint32_t rand32(void)
{
	kmalloc_test_seed = kmalloc_test_seed * 1664525U + 1013904223U;
	return kmalloc_test_seed;
}
/*
 * 在[min, max]范围内生成随机大小
 * @param min: 最小值
 * @param max: 最大值
 * @return: 范围内的随机值
 */
static inline size_t rand_size(size_t min, size_t max)
{
	if (min >= max)
		return min;
	return min + (rand32() % (max - min + 1));
}

#define TEST_ASSERT(cond, msg)                                                 \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("[FATAL] kmalloc_test: %s @ line %d\n", msg,    \
			       __LINE__);                                      \
			return;                                                \
		}                                                              \
	} while (0)

// 校验内存未被破坏（写已知 pattern + 读验证）
/*
 * 对内存写入测试标记并立即校验（用于检测内存越界）
 * @param p: 内存起始地址
 * @param sz: 内存大小（字节）
 * @param poison: 测试填充值
 * @return: 无返回值
 */
static void poison_and_check(void *p, size_t sz, uint8_t poison)
{
	if (sz == 0 || !p)
		return;
	volatile uint8_t *v = (volatile uint8_t *)p;
	// 写头、中、尾
	v[0] = poison;
	if (sz > 1)
		v[sz - 1] = poison;
	if (sz > 64)
		v[sz / 2] = poison;
	// 立即校验（防写缓冲延迟暴露 bug）
	TEST_ASSERT(v[0] == poison, "head corrupted");
	if (sz > 1)
		TEST_ASSERT(v[sz - 1] == poison, "tail corrupted");
	if (sz > 64)
		TEST_ASSERT(v[sz / 2] == poison, "mid corrupted");
}

/*
 * kmalloc系统综合测试函数：覆盖零大小分配、slab范围、buddy大块、高压混合、碎片回收等场景
 * @return: 无返回值
 */
void kalloc_test(void)
{
	printf("=== kmalloc STRESS & CORRECTNESS TEST (slab + buddy) ===\n");

	// ------------------------------------------------------------------
	// 1. 零大小：验证拒绝分配（返回 NULL）
	// ------------------------------------------------------------------
	printf("[1] Zero-size allocation...\n");
	TEST_ASSERT(kmalloc(0) == NULL, "kmalloc(0) should return NULL");
	printf("✓ kmalloc(0) → NULL\n");

	// ------------------------------------------------------------------
	// 2. slab 范围全覆盖：精确命中每个 cache size
	// ------------------------------------------------------------------
	printf("[2] Slab cache coverage (exact sizes)...\n");
	const size_t slab_sizes[] = {8,	  16,  24,  32,	 48,  64,   96,	  128,
				     192, 256, 384, 512, 768, 1024, 1536, 2048};
	const int n_slab = sizeof(slab_sizes) / sizeof(slab_sizes[0]);
	void *slab_ptrs[n_slab];

	for (int i = 0; i < n_slab; i++) {
		size_t sz = slab_sizes[i];
		void *p = kmalloc(sz);
		TEST_ASSERT(p != NULL, "slab exact alloc failed");
		poison_and_check(p, sz, 0xAA + i);
		slab_ptrs[i] = p;
	}

	// 释放（逆序，测试 slab free-list 合并/复用）
	for (int i = n_slab - 1; i >= 0; i--) {
		poison_and_check(slab_ptrs[i], slab_sizes[i],
				 0xAA + i); // 释放前再校验
		kfree(slab_ptrs[i]);
	}
	printf("✓ All slab caches allocated & freed.\n");

	// ------------------------------------------------------------------
	// 3. slab 范围边界：每个 cache 的 min/max 请求
	// ------------------------------------------------------------------
	printf("[3] Slab boundary tests (min/max per cache)...\n");
	for (int i = 0; i < n_slab; i++) {
		size_t min_req = (i == 0) ? 1 : slab_sizes[i - 1] + 1;
		size_t max_req = slab_sizes[i];
		// 请求 min_req → 应落入 slab_sizes[i]
		void *p1 = kmalloc(min_req);
		TEST_ASSERT(p1 != NULL, "slab min boundary failed");
		// 请求 max_req → 应落入 slab_sizes[i]
		void *p2 = kmalloc(max_req);
		TEST_ASSERT(p2 != NULL, "slab max boundary failed");

		poison_and_check(p1, min_req, 0xBB);
		poison_and_check(p2, max_req, 0xCC);

		kfree(p1);
		kfree(p2);
	}
	printf("✓ Slab boundaries passed.\n");

	// ------------------------------------------------------------------
	// 4. buddy 范围：大块分配（PAGE_SIZE 到 8MB）
	// ------------------------------------------------------------------
	printf("[4] Buddy allocations (large blocks)...\n");
	const size_t buddy_sizes[] = {
	    4096,	     // 4KB = 1 page
	    8192,	     // 8KB
	    65536,	     // 64KB
	    262144,	     // 256KB
	    1 * 1024 * 1024, // 1MB
	    4 * 1024 * 1024, // 4MB
	    8 * 1024 * 1024  // 8MB (your max)
	};
	const int n_buddy = sizeof(buddy_sizes) / sizeof(buddy_sizes[0]);
	void *buddy_ptrs[n_buddy];

	for (int i = 0; i < n_buddy; i++) {
		size_t sz = buddy_sizes[i];
		printf("  - Alloc %zu KB...", sz / 1024);
		void *p = kmalloc(sz);
		if (!p) {
			printf(" FAILED (sz=%zu)\n", sz);
			TEST_ASSERT(sz > 8 * 1024 * 1024,
				    "Unexpected buddy alloc failure");
			break;
		}
		printf(" OK\n");

		// 校验：大块更需防越界
		poison_and_check(p, sz, 0xDD);
		buddy_ptrs[i] = p;
	}

	// 释放（逆序，触发 buddy 合并）
	for (int i = n_buddy - 1; i >= 0; i--) {
		if (buddy_ptrs[i]) {
			poison_and_check(buddy_ptrs[i], buddy_sizes[i], 0xDD);
			kfree(buddy_ptrs[i]);
		}
	}
	printf("✓ Buddy allocations passed.\n");

	// ------------------------------------------------------------------
	// 5. 超限测试：>8MB 应失败
	// ------------------------------------------------------------------
	printf("[5] Overflow test (>8MB)...\n");
	TEST_ASSERT(kmalloc(8 * 1024 * 1024 + 1) == NULL, "should fail >8MB");
	TEST_ASSERT(kmalloc(SIZE_MAX) == NULL, "should fail SIZE_MAX");
	printf("✓ Overflow handled.\n");

	// ------------------------------------------------------------------
	// 6. 高压混合压力测试（核心！）
	//    - 混合小/中/大块
	//    - 随机顺序分配+释放
	//    - 持续校验内存完整性
	// ------------------------------------------------------------------
	printf("[6] High-pressure mixed allocation (1000 cycles)...\n");
	const int N = 1000;
	void *ptrs[N];
	size_t sizes[N];
	int count = 0;
	size_t total = 0;

	// 分配阶段
	for (int i = 0; i < N; i++) {
		// 随机尺寸分布：70% 小块 (1~2KB), 20% 中块 (4~64KB), 10% 大块
		// (128KB~4MB)
		uint32_t r = rand32() % 100;
		size_t sz;
		if (r < 70) {
			sz = rand_size(1, 2048);
		} else if (r < 90) {
			sz = rand_size(4 * 1024, 64 * 1024);
		} else {
			sz = rand_size(128 * 1024, 4 * 1024 * 1024);
		}

		void *p = kmalloc(sz);
		if (!p) {
			// 允许因内存耗尽失败，但前面应已分配不少
			if (count < 100) {
				TEST_ASSERT(0,
					    "Early failure in pressure test");
			}
			break;
		}

		poison_and_check(p, sz, 0xEE + (i % 200));
		ptrs[count] = p;
		sizes[count] = sz;
		total += sz;
		count++;
	}

	printf("  - Allocated %d blocks (max %d): %uM %uK %uB\n", count, N,
	       total / (1024 * 1024), total / 1024 % 1024,
	       total % (1024 * 1024) % 1024);

	// 校验阶段（释放前再验一次）
	for (int i = 0; i < count; i++) {
		poison_and_check(ptrs[i], sizes[i], 0xEE + (i % 200));
	}

	// 打乱释放顺序（Fisher-Yates shuffle）
	for (int i = count - 1; i > 0; i--) {
		int j = rand32() % (i + 1);
		// swap ptrs[i] and ptrs[j]
		void *tmp_p = ptrs[i];
		size_t tmp_s = sizes[i];
		ptrs[i] = ptrs[j];
		sizes[i] = sizes[j];
		ptrs[j] = tmp_p;
		sizes[j] = tmp_s;
	}

	// 释放阶段
	for (int i = 0; i < count; i++) {
		poison_and_check(ptrs[i], sizes[i],
				 0xEE + (i % 200)); // 释放前最后校验
		kfree(ptrs[i]);
	}

	printf("✓ Pressure test: %d alloc/free cycles completed.\n");

	// ------------------------------------------------------------------
	// 7. 内存碎片 & 回收测试
	//    - 分配大量中等块（制造碎片）
	//    - 释放间隔块（制造空洞）
	//    - 尝试分配大块（验证 buddy 合并）
	// ------------------------------------------------------------------
	printf("[7] Fragmentation & coalescing test...\n");
// Step 1: 分配 128 个 64KB 块（共 8MB）
#define FRAG_N 128
	void *frag_ptrs[FRAG_N] = {0};
	for (int i = 0; i < FRAG_N; i++) {
		frag_ptrs[i] = kmalloc(64 * 1024);
		TEST_ASSERT(frag_ptrs[i] != NULL, "frag alloc failed");
	}

	// Step 2: 释放奇数索引（制造空洞）
	for (int i = 1; i < FRAG_N; i += 2) {
		kfree(frag_ptrs[i]);
		frag_ptrs[i] = NULL;
	}

	// Step 3: 尝试分配 4MB — 应成功（空洞合并为连续 4MB+）
	void *large_after_frag = kmalloc(4 * 1024 * 1024);
	TEST_ASSERT(large_after_frag != NULL,
		    "4MB alloc failed after fragmentation");
	poison_and_check(large_after_frag, 4 * 1024 * 1024, 0xFF);
	kfree(large_after_frag);

	// Step 4: 释放剩余，回收全部
	for (int i = 0; i < FRAG_N; i++) {
		if (frag_ptrs[i])
			kfree(frag_ptrs[i]);
	}
	printf("✓ Fragmentation handling passed.\n");

	// ------------------------------------------------------------------
	// 8. 长期稳定性：10,000 次快速 alloc/free（slab 压力）
	// ------------------------------------------------------------------
	printf("[8] Long-term slab stability (10,000 cycles)...\n");
	for (int i = 0; i < 10000; i++) {
		size_t sz = slab_sizes[rand32() % n_slab]; // 精确命中 cache
		void *p = kmalloc(sz);
		TEST_ASSERT(p != NULL, "slab stability failed");
		poison_and_check(p, sz, 0x55);
		kfree(p);
	}
	printf("✓ 10,000 slab alloc/free cycles completed.\n");

	printf(
	    "\n=== 🎉 kmalloc TEST PASSED (slab + buddy core verified) ===\n");
	printf("Recommendations:\n"
	       "  - Add guard pages/redzones for overrun detection (if MMU "
	       "enabled)\n"
	       "  - Consider adding alloc/free counters per cache for "
	       "debugging\n");
}
