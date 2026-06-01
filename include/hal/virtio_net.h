#pragma once

#include <misc/stdint.h>

int virtio_net_send(const void *data, int len);
int virtio_net_recv(void *buf, int buf_len);
int virtio_net_get_mac(u8 mac[6]);
int virtio_net_init(void);
void virtio_net_isr(void);
