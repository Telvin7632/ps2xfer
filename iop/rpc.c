/*
 * SIF RPC server: the EE polls the status block (~4 times per second) and can
 * ask to exit (pad button). Never on the data path.
 */
#include "irx_imports.h"
#include "px.h"

u32 g_push_addr; /* EE physical address for the status push, 0 = off */

static SifRpcDataQueue_t g_qd;
static SifRpcServerData_t g_sd;
static u32 g_rpc_buf[(sizeof(px_stats_t) + 64) / 4] __attribute__((aligned(64)));

static void *rpc_handler(int fno, void *buf, int size)
{
    (void)size;
    switch (fno) {
        case PX_RPC_GET_STATS:
            g_stats.uptime_ms = px_ms();
            memcpy(buf, &g_stats, sizeof(g_stats));
            break;
        case PX_RPC_SET_PUSH:
            g_push_addr = *(u32 *)buf;
            memcpy(buf, &g_stats, sizeof(g_stats));
            break;
        case PX_RPC_SET_SUM:
            eesum_register((const u32 *)buf);
            break;
        case PX_RPC_EXIT:
            px_log("EXIT requested on the PS2\n");
            ctrl_shutdown_sync();
            memcpy(buf, &g_stats, sizeof(g_stats));
            break;
        default:
            break;
    }
    return buf;
}

void rpc_thread(void *arg)
{
    (void)arg;
    sceSifInitRpc(0);
    sceSifSetRpcQueue(&g_qd, GetThreadId());
    sceSifRegisterRpc(&g_sd, PX_RPC_SID, rpc_handler, g_rpc_buf, NULL, NULL, &g_qd);
    sceSifRpcLoop(&g_qd);
}
