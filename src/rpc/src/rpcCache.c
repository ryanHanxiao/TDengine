/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "os.h"
#include "tglobalcfg.h"
#include "tlog.h"
#include "tmempool.h"
#include "ttime.h"
#include "ttimer.h"
#include "tutil.h"
#include "rpcCache.h"

typedef struct SConnHash {
  uint32_t          ip;
  uint16_t          port;
  char              connType;
  struct SConnHash *prev;
  struct SConnHash *next;
  void             *data;
  uint64_t          time;
} SConnHash;

typedef struct {
  SConnHash     **connHashList;
  mpool_h         connHashMemPool;
  int             maxSessions;
  int             total;
  int *           count;
  int64_t         keepTimer;
  pthread_mutex_t mutex;
  void          (*cleanFp)(void *);
  void           *tmrCtrl;
  void           *pTimer;
  int64_t        *lockedBy;
} SConnCache;

static int  rpcHashConn(void *handle, uint32_t ip, uint16_t port, int8_t connType);
static void rpcLockCache(int64_t *lockedBy);
static void rpcUnlockCache(int64_t *lockedBy);
static void rpcCleanConnCache(void *handle, void *tmrId);
static void rpcRemoveExpiredNodes(SConnCache *pCache, SConnHash *pNode, int hash, uint64_t time);

void *rpcOpenConnCache(int maxSessions, void (*cleanFp)(void *), void *tmrCtrl, int64_t keepTimer) {
  SConnHash **connHashList;
  mpool_h     connHashMemPool;
  SConnCache *pCache;

  connHashMemPool = taosMemPoolInit(maxSessions, sizeof(SConnHash));
  if (connHashMemPool == 0) return NULL;

  connHashList = calloc(sizeof(SConnHash *), maxSessions);
  if (connHashList == 0) {
    taosMemPoolCleanUp(connHashMemPool);
    return NULL;
  }

  pCache = malloc(sizeof(SConnCache));
  if (pCache == NULL) {
    taosMemPoolCleanUp(connHashMemPool);
    free(connHashList);
    return NULL;
  }
  memset(pCache, 0, sizeof(SConnCache));

  pCache->count = calloc(sizeof(int), maxSessions);
  pCache->total = 0;
  pCache->keepTimer = keepTimer;
  pCache->maxSessions = maxSessions;
  pCache->connHashMemPool = connHashMemPool;
  pCache->connHashList = connHashList;
  pCache->cleanFp = cleanFp;
  pCache->tmrCtrl = tmrCtrl;
  pCache->lockedBy = calloc(sizeof(int64_t), maxSessions);
  taosTmrReset(rpcCleanConnCache, pCache->keepTimer * 2, pCache, pCache->tmrCtrl, &pCache->pTimer);

  pthread_mutex_init(&pCache->mutex, NULL);

  return pCache;
}

void rpcCloseConnCache(void *handle) {
  SConnCache *pCache;

  pCache = (SConnCache *)handle;
  if (pCache == NULL || pCache->maxSessions == 0) return;

  pthread_mutex_lock(&pCache->mutex);

  taosTmrStopA(&(pCache->pTimer));

  if (pCache->connHashMemPool) taosMemPoolCleanUp(pCache->connHashMemPool);

  tfree(pCache->connHashList);
  tfree(pCache->count);
  tfree(pCache->lockedBy);

  pthread_mutex_unlock(&pCache->mutex);

  pthread_mutex_destroy(&pCache->mutex);

  memset(pCache, 0, sizeof(SConnCache));
  free(pCache);
}

void rpcAddConnIntoCache(void *handle, void *data, uint32_t ip, uint16_t port, int8_t connType) {
  int         hash;
  SConnHash * pNode;
  SConnCache *pCache;

  uint64_t time = taosGetTimestampMs();

  pCache = (SConnCache *)handle;
  assert(pCache); 
  assert(data);

  hash = rpcHashConn(pCache, ip, port, connType);
  pNode = (SConnHash *)taosMemPoolMalloc(pCache->connHashMemPool);
  pNode->ip = ip;
  pNode->port = port;
  pNode->connType = connType;
  pNode->data = data;
  pNode->prev = NULL;
  pNode->time = time;

  rpcLockCache(pCache->lockedBy+hash);

  pNode->next = pCache->connHashList[hash];
  if (pCache->connHashList[hash] != NULL) (pCache->connHashList[hash])->prev = pNode;
  pCache->connHashList[hash] = pNode;

  pCache->count[hash]++;
  rpcRemoveExpiredNodes(pCache, pNode->next, hash, time);

  rpcUnlockCache(pCache->lockedBy+hash);

  pCache->total++;

  tTrace("%p ip:0x%x:%hu:%d:%d:%p added into cache, connections:%d", data, ip, port, connType, hash, pNode, pCache->count[hash]);

  return;
}

void *rpcGetConnFromCache(void *handle, uint32_t ip, uint16_t port, int8_t connType) {
  int         hash;
  SConnHash * pNode;
  SConnCache *pCache;
  void *      pData = NULL;

  pCache = (SConnCache *)handle;
  assert(pCache); 

  uint64_t time = taosGetTimestampMs();

  hash = rpcHashConn(pCache, ip, port, connType);
  rpcLockCache(pCache->lockedBy+hash);

  pNode = pCache->connHashList[hash];
  while (pNode) {
    if (time >= pCache->keepTimer + pNode->time) {
      rpcRemoveExpiredNodes(pCache, pNode, hash, time);
      pNode = NULL;
      break;
    }

    if (pNode->ip == ip && pNode->port == port && pNode->connType == connType) break;

    pNode = pNode->next;
  }

  if (pNode) {
    rpcRemoveExpiredNodes(pCache, pNode->next, hash, time);

    if (pNode->prev) {
      pNode->prev->next = pNode->next;
    } else {
      pCache->connHashList[hash] = pNode->next;
    }

    if (pNode->next) {
      pNode->next->prev = pNode->prev;
    }

    pData = pNode->data;
    taosMemPoolFree(pCache->connHashMemPool, (char *)pNode);
    pCache->total--;
    pCache->count[hash]--;
  }

  rpcUnlockCache(pCache->lockedBy+hash);

  if (pData) {
    tTrace("%p ip:0x%x:%hu:%d:%d:%p retrieved from cache, connections:%d", pData, ip, port, connType, hash, pNode, pCache->count[hash]);
  }

  return pData;
}

static void rpcCleanConnCache(void *handle, void *tmrId) {
  int         hash;
  SConnHash * pNode;
  SConnCache *pCache;

  pCache = (SConnCache *)handle;
  if (pCache == NULL || pCache->maxSessions == 0) return;
  if (pCache->pTimer != tmrId) return;

  uint64_t time = taosGetTimestampMs();

  for (hash = 0; hash < pCache->maxSessions; ++hash) {
    rpcLockCache(pCache->lockedBy+hash);
    pNode = pCache->connHashList[hash];
    rpcRemoveExpiredNodes(pCache, pNode, hash, time);
    rpcUnlockCache(pCache->lockedBy+hash);
  }

  // tTrace("timer, total connections in cache:%d", pCache->total);
  taosTmrReset(rpcCleanConnCache, pCache->keepTimer * 2, pCache, pCache->tmrCtrl, &pCache->pTimer);
}

static void rpcRemoveExpiredNodes(SConnCache *pCache, SConnHash *pNode, int hash, uint64_t time) {
  if (pNode == NULL || (time < pCache->keepTimer + pNode->time) ) return;

  SConnHash *pPrev = pNode->prev, *pNext;

  while (pNode) {
    (*pCache->cleanFp)(pNode->data);
    pNext = pNode->next;
    pCache->total--;
    pCache->count[hash]--;
    tTrace("%p ip:0x%x:%hu:%d:%d:%p removed from cache, connections:%d", pNode->data, pNode->ip, pNode->port, pNode->connType, hash, pNode,
             pCache->count[hash]);
    taosMemPoolFree(pCache->connHashMemPool, (char *)pNode);
    pNode = pNext;
  }

  if (pPrev)
    pPrev->next = NULL;
  else
    pCache->connHashList[hash] = NULL;
}

static int rpcHashConn(void *handle, uint32_t ip, uint16_t port, int8_t connType) {
  SConnCache *pCache = (SConnCache *)handle;
  int         hash = 0;

  hash = ip >> 16;
  hash += (unsigned short)(ip & 0xFFFF);
  hash += port;
  hash += connType;

  hash = hash % pCache->maxSessions;

  return hash;
}

static void rpcLockCache(int64_t *lockedBy) {
  int64_t tid = taosGetPthreadId();
  int     i = 0;
  while (atomic_val_compare_exchange_64(lockedBy, 0, tid) != 0) {
    if (++i % 100 == 0) {
      sched_yield();
    }
  }
}

static void rpcUnlockCache(int64_t *lockedBy) {
  int64_t tid = taosGetPthreadId();
  if (atomic_val_compare_exchange_64(lockedBy, tid, 0) != tid) {
    assert(false);
  }
}

