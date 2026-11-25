#ifndef PX_EESUM_H
#define PX_EESUM_H

#include <sifrpc.h>

/* start the verify checksum helper and register it with the IOP module;
   main_priority: the calling (screen) thread's priority. 0 or -1. */
int eesum_start(SifRpcClientData_t *rpc, int main_priority);

#endif
