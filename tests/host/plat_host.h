#ifndef PLAT_HOST_H
#define PLAT_HOST_H

#include "px.h"

extern int g_img_fd;
extern u64 g_img_sectors;
extern int g_sock;
extern u32 g_host_maxfree;
extern u32 g_disk_mbps;
extern double g_drop_tx_pct;

void plat_host_init(void);

#endif
