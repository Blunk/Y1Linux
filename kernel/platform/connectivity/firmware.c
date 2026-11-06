// SPDX-License-Identifier: GPL-2.0-only
/* Exact owner-supplied inputs from the completed firmware inventory. A
 * mismatched blob fails before any firmware bytes reach the controller. */
#include <crypto/sha2.h>
#include <linux/firmware.h>
#include "conn.h"
static const struct { const char *name; size_t size; u8 hash[32]; } inputs[] = Y2_CONN_FIRMWARE;
int y2_conn_request_firmware(struct device *dev, const char *name, const struct firmware **fw)
{
 unsigned i; u8 hash[32]; int ret;
 *fw=NULL;
 for (i=0;i<ARRAY_SIZE(inputs);i++) if (!strcmp(name,inputs[i].name)) break;
 if (i==ARRAY_SIZE(inputs)) return -ENOENT;
 ret=request_firmware(fw,name,dev);
 if (ret) return ret;
 if ((*fw)->size==inputs[i].size) {
  sha256((*fw)->data,(*fw)->size,hash);
  if (!memcmp(hash,inputs[i].hash,sizeof(hash))) return 0;
 }
 release_firmware(*fw); *fw=NULL;
 dev_err(dev,"owner firmware identity mismatch: %s\n",name);
 return -EKEYREJECTED;
}
