#include <misc/string.h>

/*
 * 内存复制，将 src 指向的内存复制 n 字节到 dest
 * @param dest: 目标内存地址
 * @param src: 源内存地址
 * @param n: 复制的字节数
 * @return: 目标内存地址 dest
 */
void *memcpy(void *dest, const void *src, size_t n) {
  char *d = (char *)dest;
  const char *s = (const char *)src;

  for (size_t i = 0; i < n; i++) {
    d[i] = s[i];
  }

  return dest;
}

/*
 * 内存移动（处理源和目标重叠的情况）
 * 若 dest < src 则前向复制，否则后向复制
 * @param dest: 目标内存地址
 * @param src: 源内存地址
 * @param n: 移动的字节数
 * @return: 目标内存地址 dest
 */
void *memmove(void *dest, const void *src, size_t n) {
  char *d = (char *)dest;
  const char *s = (const char *)src;

  if (d < s) {
    // 前向复制
    for (size_t i = 0; i < n; i++) {
      d[i] = s[i];
    }
  } else if (d > s) {
    // 后向复制
    for (size_t i = n; i > 0; i--) {
      d[i - 1] = s[i - 1];
    }
  }

  return dest;
}

/*
 * 内存设置，将指定内存区域填充为指定字节值
 * @param s: 目标内存地址
 * @param c: 填充的字节值（以 int 传入，转为 unsigned char）
 * @param n: 填充的字节数
 * @return: 目标内存地址 s
 */
void *memset(void *s, int c, size_t n) {
  unsigned char *p = (unsigned char *)s;
  unsigned char ch = (unsigned char)c;

  for (size_t i = 0; i < n; i++) {
    p[i] = ch;
  }

  return s;
}

/*
 * 内存比较，逐字节比较两块内存区域
 * @param s1: 第一块内存地址
 * @param s2: 第二块内存地址
 * @param n: 比较的字节数
 * @return: 相等返回 0；若 s1 < s2 返回负值，否则返回正值
 */
int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *p1 = (const unsigned char *)s1;
  const unsigned char *p2 = (const unsigned char *)s2;

  for (size_t i = 0; i < n; i++) {
    if (p1[i] != p2[i]) {
      return p1[i] - p2[i];
    }
  }

  return 0;
}

/*
 * 计算字符串长度（不含终止符 \0）
 * @param s: 输入字符串
 * @return: 字符串长度
 */
size_t strlen(const char *s) {
  size_t len = 0;

  while (s[len] != '\0') {
    len++;
  }

  return len;
}

/*
 * 计算字符串长度（带最大长度限制）
 * @param s: 输入字符串
 * @param maxlen: 最大遍历长度
 * @return: 字符串长度（不超过 maxlen）
 */
size_t strnlen(const char *s, size_t maxlen) {
  size_t len = 0;

  while (len < maxlen && s[len] != '\0') {
    len++;
  }

  return len;
}

/*
 * 字符串复制，将 src 复制到 dest（含终止符 \0）
 * @param dest: 目标缓冲区
 * @param src: 源字符串
 * @return: 目标缓冲区 dest
 */
char *strcpy(char *dest, const char *src) {
  size_t i = 0;

  while (src[i] != '\0') {
    dest[i] = src[i];
    i++;
  }
  dest[i] = '\0';

  return dest;
}

/*
 * 字符串复制（带长度限制），若 src 长度小于 n 则用 \0 填充剩余空间
 * @param dest: 目标缓冲区
 * @param src: 源字符串
 * @param n: 最大复制字符数
 * @return: 目标缓冲区 dest
 */
char *strncpy(char *dest, const char *src, size_t n) {
  size_t i = 0;

  while (i < n && src[i] != '\0') {
    dest[i] = src[i];
    i++;
  }

  // 用 '\0' 填充剩余空间
  while (i < n) {
    dest[i] = '\0';
    i++;
  }

  return dest;
}

/*
 * 字符串连接，将 src 追加到 dest 末尾
 * @param dest: 目标字符串（必须有足够空间）
 * @param src: 要追加的源字符串
 * @return: 目标字符串 dest
 */
char *strcat(char *dest, const char *src) {
  size_t dest_len = strlen(dest);
  size_t i = 0;

  while (src[i] != '\0') {
    dest[dest_len + i] = src[i];
    i++;
  }
  dest[dest_len + i] = '\0';

  return dest;
}

/*
 * 字符串连接（带长度限制），将 src 最多 n 个字符追加到 dest 末尾
 * @param dest: 目标字符串（必须有足够空间）
 * @param src: 要追加的源字符串
 * @param n: 最大追加字符数
 * @return: 目标字符串 dest
 */
char *strncat(char *dest, const char *src, size_t n) {
  size_t dest_len = strlen(dest);
  size_t i = 0;

  while (i < n && src[i] != '\0') {
    dest[dest_len + i] = src[i];
    i++;
  }
  dest[dest_len + i] = '\0';

  return dest;
}

/*
 * 字符串比较，逐字符比较两个字符串
 * @param s1: 第一个字符串
 * @param s2: 第二个字符串
 * @return: 相等返回 0；s1 < s2 返回负值，s1 > s2 返回正值
 */
int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }

  return *(unsigned char *)s1 - *(unsigned char *)s2;
}

/*
 * 字符串比较（带长度限制），最多比较 n 个字符
 * @param s1: 第一个字符串
 * @param s2: 第二个字符串
 * @param n: 最大比较字符数
 * @return: 相等返回 0；s1 < s2 返回负值，s1 > s2 返回正值
 */
int strncmp(const char *s1, const char *s2, size_t n) {
  if (n == 0)
    return 0;

  while (n > 1 && *s1 && (*s1 == *s2)) {
    s1++;
    s2++;
    n--;
  }

  return *(unsigned char *)s1 - *(unsigned char *)s2;
}

/*
 * 在字符串中查找指定字符的第一次出现
 * @param s: 输入字符串
 * @param c: 要查找的字符（以 int 传入）
 * @return: 指向第一次出现位置的指针，未找到返回 NULL
 */
char *strchr(const char *s, int c) {
  while (*s != (char)c) {
    if (*s == '\0') {
      return NULL;
    }
    s++;
  }

  return (char *)s;
}

/*
 * 在字符串中查找指定字符的最后一次出现
 * @param s: 输入字符串
 * @param c: 要查找的字符（以 int 传入）
 * @return: 指向最后一次出现位置的指针，未找到返回 NULL
 */
char *strrchr(const char *s, int c) {
  const char *last = NULL;

  do {
    if (*s == (char)c) {
      last = s;
    }
  } while (*s++);

  return (char *)last;
}

/*
 * 在字符串中查找子字符串的第一次出现
 * @param haystack: 被搜索的字符串
 * @param needle: 要查找的子字符串
 * @return: 指向第一次出现位置的指针，未找到返回 NULL
 */
char *strstr(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;

  size_t needle_len = strlen(needle);
  size_t haystack_len = strlen(haystack);

  if (needle_len > haystack_len)
    return NULL;

  for (size_t i = 0; i <= haystack_len - needle_len; i++) {
    if (memcmp(haystack + i, needle, needle_len) == 0) {
      return (char *)(haystack + i);
    }
  }

  return NULL;
}

/*
 * 计算字符串起始连续包含指定字符集的长度
 * @param s: 输入字符串
 * @param accept: 允许的字符集
 * @return: 前缀中只包含 accept 字符的长度
 */
size_t strspn(const char *s, const char *accept) {
  size_t count = 0;

  while (*s && strchr(accept, *s)) {
    count++;
    s++;
  }

  return count;
}

/*
 * 计算字符串起始连续不包含指定字符集的长度
 * @param s: 输入字符串
 * @param reject: 排除的字符集
 * @return: 前缀中不包含 reject 字符的长度
 */
size_t strcspn(const char *s, const char *reject) {
  size_t count = 0;

  while (*s && !strchr(reject, *s)) {
    count++;
    s++;
  }

  return count;
}

/*
 * 在字符串中查找第一个出现在指定字符集中的字符
 * @param s: 输入字符串
 * @param accept: 要查找的字符集
 * @return: 指向第一个匹配字符的指针，未找到返回 NULL
 */
char *strpbrk(const char *s, const char *accept) {
  while (*s) {
    if (strchr(accept, *s)) {
      return (char *)s;
    }
    s++;
  }

  return NULL;
}

/*
 * 字符串分割，按分隔符将字符串分割为令牌（线程不安全，使用静态缓冲区）
 * @param str: 待分割的字符串（首次调用传入，后续传入 NULL）
 * @param delim: 分隔符字符集
 * @return: 下一个令牌的指针，无更多令牌时返回 NULL
 */
char *strtok(char *str, const char *delim) {
  static char *last = NULL;
  char *token;

  if (str) {
    last = str;
  } else if (!last) {
    return NULL;
  }

  // 跳过前导分隔符
  while (*last && strchr(delim, *last)) {
    last++;
  }

  if (!*last) {
    last = NULL;
    return NULL;
  }

  token = last;

  // 找到下一个分隔符
  while (*last && !strchr(delim, *last)) {
    last++;
  }

  if (*last) {
    *last = '\0';
    last++;
  } else {
    last = NULL;
  }

  return token;
}

/*
 * 在内存块中查找指定字符的第一次出现
 * @param s: 内存起始地址
 * @param c: 要查找的字符（以 int 传入）
 * @param n: 搜索的字节数
 * @return: 指向第一次出现位置的指针，未找到返回 NULL
 */
void *memchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  unsigned char ch = (unsigned char)c;

  for (size_t i = 0; i < n; i++) {
    if (p[i] == ch) {
      return (void *)(p + i);
    }
  }

  return NULL;
}

// // 复制字符串（动态分配）
// char *strdup(const char *s) {
//   if (!s)
//     return NULL;

//   size_t len = strlen(s) + 1;
//   char *new_str = (char *)malloc(len); // 注意：内核中需要实现自己的malloc

//   if (new_str) {
//     memcpy(new_str, s, len);
//   }

//   return new_str;
// }

// // 复制字符串（带长度限制）
// char *strndup(const char *s, size_t n) {
//   if (!s)
//     return NULL;

//   size_t len = strnlen(s, n);
//   char *new_str = (char *)malloc(len + 1); //
//   注意：内核中需要实现自己的malloc

//   if (new_str) {
//     memcpy(new_str, s, len);
//     new_str[len] = '\0';
//   }

//   return new_str;
// }
