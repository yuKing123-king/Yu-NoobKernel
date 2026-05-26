#include <misc/string.h>

// 内存复制
void *memcpy(void *dest, const void *src, size_t n) {
  char *d = (char *)dest;
  const char *s = (const char *)src;

  for (size_t i = 0; i < n; i++) {
    d[i] = s[i];
  }

  return dest;
}

// 内存移动（处理重叠内存）
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

// 内存设置
void *memset(void *s, int c, size_t n) {
  unsigned char *p = (unsigned char *)s;
  unsigned char ch = (unsigned char)c;

  for (size_t i = 0; i < n; i++) {
    p[i] = ch;
  }

  return s;
}

// 内存比较
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

// 计算字符串长度
size_t strlen(const char *s) {
  size_t len = 0;

  while (s[len] != '\0') {
    len++;
  }

  return len;
}

// 计算字符串长度（带最大长度限制）
size_t strnlen(const char *s, size_t maxlen) {
  size_t len = 0;

  while (len < maxlen && s[len] != '\0') {
    len++;
  }

  return len;
}

// 字符串复制
char *strcpy(char *dest, const char *src) {
  size_t i = 0;

  while (src[i] != '\0') {
    dest[i] = src[i];
    i++;
  }
  dest[i] = '\0';

  return dest;
}

// 字符串复制（带长度限制）
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

// 字符串连接
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

// 字符串连接（带长度限制）
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

// 字符串比较
int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }

  return *(unsigned char *)s1 - *(unsigned char *)s2;
}

// 字符串比较（带长度限制）
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

// 查找字符（第一次出现）
char *strchr(const char *s, int c) {
  while (*s != (char)c) {
    if (*s == '\0') {
      return NULL;
    }
    s++;
  }

  return (char *)s;
}

// 查找字符（最后一次出现）
char *strrchr(const char *s, int c) {
  const char *last = NULL;

  do {
    if (*s == (char)c) {
      last = s;
    }
  } while (*s++);

  return (char *)last;
}

// 查找子字符串
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

// 计算字符串前缀长度（只包含指定字符）
size_t strspn(const char *s, const char *accept) {
  size_t count = 0;

  while (*s && strchr(accept, *s)) {
    count++;
    s++;
  }

  return count;
}

// 计算字符串前缀长度（不包含指定字符）
size_t strcspn(const char *s, const char *reject) {
  size_t count = 0;

  while (*s && !strchr(reject, *s)) {
    count++;
    s++;
  }

  return count;
}

// 查找字符串中第一个在accept中出现的字符
char *strpbrk(const char *s, const char *accept) {
  while (*s) {
    if (strchr(accept, *s)) {
      return (char *)s;
    }
    s++;
  }

  return NULL;
}

// 字符串分割
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

// 在内存中查找字符
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
