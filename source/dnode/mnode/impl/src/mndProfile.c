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

#define _DEFAULT_SOURCE
#include "mndProfile.h"
#include "mndDb.h"
#include "mndMnode.h"
#include "mndShow.h"
#include "mndUser.h"

#define QUERY_ID_SIZE 20
#define QUERY_OBJ_ID_SIZE 18
#define SUBQUERY_INFO_SIZE 6
#define QUERY_STREAM_SAVE_SIZE 20

typedef struct {
  char         user[TSDB_USER_LEN];
  char         app[TSDB_APP_NAME_LEN];  // app name that invokes taosc
  int32_t      pid;                     // pid of app that invokes taosc
  int32_t      id;
  int8_t       killed;
  int8_t       align;
  uint16_t     port;
  uint32_t     ip;
  int64_t      stime;
  int64_t      lastAccess;
  int32_t      queryId;
  int32_t      streamId;
  int32_t      numOfQueries;
  int32_t      numOfStreams;
  SStreamDesc *pStreams;
  SQueryDesc  *pQueries;
} SConnObj;

static SConnObj *mndCreateConn(SMnode *pMnode, char *user, uint32_t ip, uint16_t port, int32_t pid, const char *app);
static void      mndFreeConn(SConnObj *pConn);
static SConnObj *mndAcquireConn(SMnode *pMnode, int32_t connId);
static void      mndReleaseConn(SMnode *pMnode, SConnObj *pConn);
static void     *mndGetNextConn(SMnode *pMnode, void *pIter, SConnObj **pConn);
static void      mndCancelGetNextConn(SMnode *pMnode, void *pIter);
static int32_t   mndProcessHeartBeatMsg(SMnodeMsg *pMsg);
static int32_t   mndProcessConnectMsg(SMnodeMsg *pMsg);
static int32_t   mndProcessKillQueryMsg(SMnodeMsg *pMsg);
static int32_t   mndProcessKillStreamMsg(SMnodeMsg *pMsg);
static int32_t   mndProcessKillConnectionMsg(SMnodeMsg *pMsg);
static int32_t   mndGetConnsMeta(SMnodeMsg *pMsg, SShowObj *pShow, STableMetaMsg *pMeta);
static int32_t   mndRetrieveConns(SMnodeMsg *pMsg, SShowObj *pShow, char *data, int32_t rows);
static int32_t   mndGetQueryMeta(SMnodeMsg *pMsg, SShowObj *pShow, STableMetaMsg *pMeta);
static int32_t   mndRetrieveQueries(SMnodeMsg *pMsg, SShowObj *pShow, char *data, int32_t rows);
static void      mndCancelGetNextQuery(SMnode *pMnode, void *pIter);
static int32_t   mndGetStreamMeta(SMnodeMsg *pMsg, SShowObj *pShow, STableMetaMsg *pMeta);
static int32_t   mndRetrieveStreams(SMnodeMsg *pMsg, SShowObj *pShow, char *data, int32_t rows);
static void      mndCancelGetNextStream(SMnode *pMnode, void *pIter);

int32_t mndInitProfile(SMnode *pMnode) {
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;

  int32_t connCheckTime = pMnode->cfg.shellActivityTimer * 2;
  pMgmt->cache = taosCacheInit(TSDB_DATA_TYPE_INT, connCheckTime, true, (__cache_free_fn_t)mndFreeConn, "conn");
  if (pMgmt->cache == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    mError("failed to alloc profile cache since %s", terrstr());
    return -1;
  }

  mndSetMsgHandle(pMnode, TSDB_MSG_TYPE_HEARTBEAT, mndProcessHeartBeatMsg);
  mndSetMsgHandle(pMnode, TSDB_MSG_TYPE_CONNECT, mndProcessConnectMsg);
  mndSetMsgHandle(pMnode, TSDB_MSG_TYPE_KILL_QUERY, mndProcessKillQueryMsg);
  mndSetMsgHandle(pMnode, TSDB_MSG_TYPE_KILL_STREAM, mndProcessKillStreamMsg);
  mndSetMsgHandle(pMnode, TSDB_MSG_TYPE_KILL_CONN, mndProcessKillConnectionMsg);

  mndAddShowMetaHandle(pMnode, TSDB_MGMT_TABLE_CONNS, mndGetConnsMeta);
  mndAddShowRetrieveHandle(pMnode, TSDB_MGMT_TABLE_CONNS, mndRetrieveConns);
  mndAddShowFreeIterHandle(pMnode, TSDB_MGMT_TABLE_CONNS, mndCancelGetNextConn);
  mndAddShowMetaHandle(pMnode, TSDB_MGMT_TABLE_QUERIES, mndGetQueryMeta);
  mndAddShowRetrieveHandle(pMnode, TSDB_MGMT_TABLE_QUERIES, mndRetrieveQueries);
  mndAddShowFreeIterHandle(pMnode, TSDB_MGMT_TABLE_QUERIES, mndCancelGetNextQuery);
  mndAddShowMetaHandle(pMnode, TSDB_MGMT_TABLE_STREAMS, mndGetStreamMeta);
  mndAddShowRetrieveHandle(pMnode, TSDB_MGMT_TABLE_STREAMS, mndRetrieveStreams);
  mndAddShowFreeIterHandle(pMnode, TSDB_MGMT_TABLE_STREAMS, mndCancelGetNextStream);

  return 0;
}

void mndCleanupProfile(SMnode *pMnode) {
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;
  if (pMgmt->cache != NULL) {
    taosCacheCleanup(pMgmt->cache);
    pMgmt->cache = NULL;
  }
}

static SConnObj *mndCreateConn(SMnode *pMnode, char *user, uint32_t ip, uint16_t port, int32_t pid, const char *app) {
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;

  int32_t connId = atomic_add_fetch_32(&pMgmt->connId, 1);
  if (connId == 0) atomic_add_fetch_32(&pMgmt->connId, 1);

  SConnObj connObj = {.pid = pid,
                      .id = connId,
                      .killed = 0,
                      .port = port,
                      .ip = ip,
                      .stime = taosGetTimestampMs(),
                      .lastAccess = 0,
                      .queryId = 0,
                      .streamId = 0,
                      .numOfQueries = 0,
                      .numOfStreams = 0,
                      .pStreams = NULL,
                      .pQueries = NULL};

  connObj.lastAccess = connObj.stime;
  tstrncpy(connObj.user, user, TSDB_USER_LEN);
  tstrncpy(connObj.app, app, TSDB_APP_NAME_LEN);

  int32_t   keepTime = pMnode->cfg.shellActivityTimer * 3;
  SConnObj *pConn = taosCachePut(pMgmt->cache, &connId, sizeof(int32_t), &connObj, sizeof(connObj), keepTime * 1000);
  if (pConn == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    mError("conn:%d, data:%p failed to put into cache since %s, user:%s", connId, pConn, user, terrstr());
    return NULL;
  } else {
    mTrace("conn:%d, data:%p created, user:%s", pConn->id, pConn, user);
    return pConn;
  }
}

static void mndFreeConn(SConnObj *pConn) {
  tfree(pConn->pQueries);
  tfree(pConn->pStreams);
  mTrace("conn:%d, data:%p destroyed", pConn->id, pConn);
}

static SConnObj *mndAcquireConn(SMnode *pMnode, int32_t connId) {
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;

  SConnObj *pConn = taosCacheAcquireByKey(pMgmt->cache, &connId, sizeof(int32_t));
  if (pConn == NULL) {
    mDebug("conn:%d, already destroyed", connId);
    return NULL;
  }

  int32_t keepTime = pMnode->cfg.shellActivityTimer * 3;
  pConn->lastAccess = keepTime * 1000 + (uint64_t)taosGetTimestampMs();

  mTrace("conn:%d, data:%p acquired from cache", pConn->id, pConn);
  return pConn;
}

static void mndReleaseConn(SMnode *pMnode, SConnObj *pConn) {
  if (pConn == NULL) return;
  mTrace("conn:%d, data:%p released from cache", pConn->id, pConn);

  SProfileMgmt *pMgmt = &pMnode->profileMgmt;
  taosCacheRelease(pMgmt->cache, (void **)&pConn, false);
}

static void *mndGetNextConn(SMnode *pMnode, void *pIter, SConnObj **pConn) {
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;

  *pConn = NULL;

  pIter = taosHashIterate(pMgmt->cache->pHashTable, pIter);
  if (pIter == NULL) return NULL;

  SCacheDataNode **pNode = pIter;
  if (pNode == NULL || *pNode == NULL) {
    taosHashCancelIterate(pMgmt->cache->pHashTable, pIter);
    return NULL;
  }

  *pConn = (SConnObj *)((*pNode)->data);
  return pIter;
}

static void mndCancelGetNextConn(SMnode *pMnode, void *pIter) {
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;
  taosHashCancelIterate(pMgmt->cache->pHashTable, pIter);
}

static int32_t mndProcessConnectMsg(SMnodeMsg *pMsg) {
  SMnode      *pMnode = pMsg->pMnode;
  SConnectMsg *pReq = pMsg->rpcMsg.pCont;
  pReq->pid = htonl(pReq->pid);

  SRpcConnInfo info = {0};
  if (rpcGetConnInfo(pMsg->rpcMsg.handle, &info) != 0) {
    mError("user:%s, failed to login while get connection info since %s", pMsg->user, terrstr());
    return -1;
  }

  char ip[30];
  taosIp2String(info.clientIp, ip);

  if (pReq->db[0]) {
    snprintf(pMsg->db, TSDB_FULL_DB_NAME_LEN, "%d%s%s", pMsg->acctId, TS_PATH_DELIMITER, pReq->db);
    SDbObj *pDb = mndAcquireDb(pMnode, pMsg->db);
    if (pDb == NULL) {
      terrno = TSDB_CODE_MND_INVALID_DB;
      mError("user:%s, failed to login from %s while use db:%s since %s", pMsg->user, ip, pReq->db, terrstr());
      return -1;
    }
    mndReleaseDb(pMnode, pDb);
  }

  SConnObj *pConn = mndCreateConn(pMnode, info.user, info.clientIp, info.clientPort, pReq->pid, pReq->app);
  if (pConn == NULL) {
    mError("user:%s, failed to login from %s while create connection since %s", pMsg->user, ip, terrstr());
    return -1;
  }

  SConnectRsp *pRsp = rpcMallocCont(sizeof(SConnectRsp));
  if (pRsp == NULL) {
    mndReleaseConn(pMnode, pConn);
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    mError("user:%s, failed to login from %s while create rsp since %s", pMsg->user, ip, terrstr());
    return -1;
  }

  SUserObj *pUser = mndAcquireUser(pMnode, pMsg->user);
  if (pUser != NULL) {
    pRsp->acctId = htonl(pUser->acctId);
    pRsp->superAuth = pUser->superAuth;
    pRsp->readAuth = pUser->readAuth;
    pRsp->writeAuth = pUser->writeAuth;
    mndReleaseUser(pMnode, pUser);
  }

  pRsp->clusterId = htonl(pMnode->clusterId);
  pRsp->connId = htonl(pConn->id);
  mndGetMnodeEpSet(pMnode, &pRsp->epSet);
  mndReleaseConn(pMnode, pConn);

  pMsg->contLen = sizeof(SConnectRsp);
  pMsg->pCont = pRsp;
  mDebug("user:%s, login from %s, conn:%d", info.user, ip, pConn->id);
  return 0;
}

static int32_t mnodeSaveQueryStreamList(SConnObj *pConn, SHeartBeatMsg *pMsg) {
  pConn->numOfQueries = 0;
  pConn->numOfStreams = 0;
  int32_t numOfQueries = htonl(pMsg->numOfQueries);
  int32_t numOfStreams = htonl(pMsg->numOfStreams);

  if (numOfQueries > 0) {
    if (pConn->pQueries == NULL) {
      pConn->pQueries = calloc(sizeof(SQueryDesc), QUERY_STREAM_SAVE_SIZE);
    }

    pConn->numOfQueries = MIN(QUERY_STREAM_SAVE_SIZE, numOfQueries);

    int32_t saveSize = pConn->numOfQueries * sizeof(SQueryDesc);
    if (saveSize > 0 && pConn->pQueries != NULL) {
      memcpy(pConn->pQueries, pMsg->pData, saveSize);
    }
  }

  if (numOfStreams > 0) {
    if (pConn->pStreams == NULL) {
      pConn->pStreams = calloc(sizeof(SStreamDesc), QUERY_STREAM_SAVE_SIZE);
    }

    pConn->numOfStreams = MIN(QUERY_STREAM_SAVE_SIZE, numOfStreams);

    int32_t saveSize = pConn->numOfStreams * sizeof(SStreamDesc);
    if (saveSize > 0 && pConn->pStreams != NULL) {
      memcpy(pConn->pStreams, pMsg->pData + numOfQueries * sizeof(SQueryDesc), saveSize);
    }
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t mndProcessHeartBeatMsg(SMnodeMsg *pMsg) {
  SMnode       *pMnode = pMsg->pMnode;
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;

  SHeartBeatMsg *pReq = pMsg->rpcMsg.pCont;
  pReq->connId = htonl(pReq->connId);
  pReq->pid = htonl(pReq->pid);

  SRpcConnInfo info = {0};
  if (rpcGetConnInfo(pMsg->rpcMsg.handle, &info) != 0) {
    mError("user:%s, connId:%d failed to process hb since %s", pMsg->user, pReq->connId, terrstr());
    return -1;
  }

  SConnObj *pConn = mndAcquireConn(pMnode, pReq->connId);
  if (pConn == NULL) {
    pConn = mndCreateConn(pMnode, info.user, info.clientIp, info.clientPort, pReq->pid, pReq->app);
    if (pConn == NULL) {
      mError("user:%s, conn:%d is freed and failed to create new conn since %s", pMsg->user, pReq->connId, terrstr());
      return -1;
    } else {
      mDebug("user:%s, conn:%d is freed and create a new conn:%d", pMsg->user, pReq->connId, pConn->id);
    }
  } else if (pConn->killed) {
    mError("user:%s, conn:%d is already killed", pMsg->user, pConn->id);
    terrno = TSDB_CODE_MND_INVALID_CONNECTION;
    return -1;
  } else {
    if (pConn->ip != info.clientIp || pConn->port != info.clientPort /* || strcmp(pConn->user, info.user) != 0 */) {
      char oldIpStr[40];
      char newIpStr[40];
      taosIpPort2String(pConn->ip, pConn->port, oldIpStr);
      taosIpPort2String(info.clientIp, info.clientPort, newIpStr);
      mError("conn:%d, incoming conn user:%s ip:%s, not match exist user:%s ip:%s", pConn->id, info.user, newIpStr,
             pConn->user, oldIpStr);

      if (pMgmt->connId < pConn->id) pMgmt->connId = pConn->id + 1;
      taosCacheRelease(pMgmt->cache, (void **)&pConn, false);
      terrno = TSDB_CODE_MND_INVALID_CONNECTION;
      return -1;
    }
  }

  SHeartBeatRsp *pRsp = rpcMallocCont(sizeof(SHeartBeatRsp));
  if (pRsp == NULL) {
    mndReleaseConn(pMnode, pConn);
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    mError("user:%s, conn:%d failed to process hb while create rsp since %s", pMsg->user, pReq->connId, terrstr());
    return -1;
  }

  mnodeSaveQueryStreamList(pConn, pReq);
  if (pConn->killed != 0) {
    pRsp->killConnection = 1;
  }

  if (pConn->streamId != 0) {
    pRsp->streamId = htonl(pConn->streamId);
    pConn->streamId = 0;
  }

  if (pConn->queryId != 0) {
    pRsp->queryId = htonl(pConn->queryId);
    pConn->queryId = 0;
  }

  pRsp->connId = htonl(pConn->id);
  pRsp->totalDnodes = htonl(1);
  pRsp->onlineDnodes = htonl(1);
  mndGetMnodeEpSet(pMnode, &pRsp->epSet);
  mndReleaseConn(pMnode, pConn);

  pMsg->contLen = sizeof(SConnectRsp);
  pMsg->pCont = pRsp;
  return 0;
}

static int32_t mndProcessKillQueryMsg(SMnodeMsg *pMsg) {
  SMnode       *pMnode = pMsg->pMnode;
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;

  SUserObj *pUser = mndAcquireUser(pMnode, pMsg->user);
  if (pUser == NULL) return 0;
  if (!pUser->superAuth) {
    mndReleaseUser(pMnode, pUser);
    terrno = TSDB_CODE_MND_NO_RIGHTS;
    return -1;
  }
  mndReleaseUser(pMnode, pUser);

  SKillQueryMsg *pKill = pMsg->rpcMsg.pCont;
  int32_t        connId = htonl(pKill->connId);
  int32_t        queryId = htonl(pKill->queryId);
  mInfo("kill query msg is received, queryId:%d", pKill->queryId);

  SConnObj *pConn = taosCacheAcquireByKey(pMgmt->cache, &connId, sizeof(int32_t));
  if (pConn == NULL) {
    mError("connId:%d, failed to kill queryId:%d, conn not exist", connId, queryId);
    terrno = TSDB_CODE_MND_INVALID_CONN_ID;
    return -1;
  } else {
    mInfo("connId:%d, queryId:%d is killed by user:%s", connId, queryId, pMsg->user);
    pConn->queryId = queryId;
    taosCacheRelease(pMgmt->cache, (void **)&pConn, false);
    return 0;
  }
}

static int32_t mndProcessKillStreamMsg(SMnodeMsg *pMsg) {
  SMnode       *pMnode = pMsg->pMnode;
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;

  SUserObj *pUser = mndAcquireUser(pMnode, pMsg->user);
  if (pUser == NULL) return 0;
  if (!pUser->superAuth) {
    mndReleaseUser(pMnode, pUser);
    terrno = TSDB_CODE_MND_NO_RIGHTS;
    return -1;
  }
  mndReleaseUser(pMnode, pUser);

  SKillStreamMsg *pKill = pMsg->rpcMsg.pCont;
  int32_t         connId = htonl(pKill->connId);
  int32_t         streamId = htonl(pKill->streamId);
  mDebug("kill stream msg is received, streamId:%d", streamId);

  SConnObj *pConn = taosCacheAcquireByKey(pMgmt->cache, &connId, sizeof(int32_t));
  if (pConn == NULL) {
    mError("connId:%d, failed to kill streamId:%d, conn not exist", connId, streamId);
    terrno = TSDB_CODE_MND_INVALID_CONN_ID;
    return -1;
  } else {
    mInfo("connId:%d, streamId:%d is killed by user:%s", connId, streamId, pMsg->user);
    pConn->streamId = streamId;
    taosCacheRelease(pMgmt->cache, (void **)&pConn, false);
    return TSDB_CODE_SUCCESS;
  }
}

static int32_t mndProcessKillConnectionMsg(SMnodeMsg *pMsg) {
  SMnode       *pMnode = pMsg->pMnode;
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;

  SUserObj *pUser = mndAcquireUser(pMnode, pMsg->user);
  if (pUser == NULL) return 0;
  if (!pUser->superAuth) {
    mndReleaseUser(pMnode, pUser);
    terrno = TSDB_CODE_MND_NO_RIGHTS;
    return -1;
  }
  mndReleaseUser(pMnode, pUser);

  SKillConnMsg *pKill = pMsg->rpcMsg.pCont;
  int32_t       connId = htonl(pKill->connId);

  SConnObj *pConn = taosCacheAcquireByKey(pMgmt->cache, &connId, sizeof(int32_t));
  if (pConn == NULL) {
    mError("connId:%d, failed to kill connection, conn not exist", connId);
    terrno = TSDB_CODE_MND_INVALID_CONN_ID;
    return -1;
  } else {
    mInfo("connId:%d, is killed by user:%s", connId, pMsg->user);
    pConn->killed = 1;
    taosCacheRelease(pMgmt->cache, (void **)&pConn, false);
    return TSDB_CODE_SUCCESS;
  }
}

static int32_t mndGetConnsMeta(SMnodeMsg *pMsg, SShowObj *pShow, STableMetaMsg *pMeta) {
  SMnode       *pMnode = pMsg->pMnode;
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;

  SUserObj *pUser = mndAcquireUser(pMnode, pMsg->user);
  if (pUser == NULL) return 0;
  if (!pUser->superAuth) {
    mndReleaseUser(pMnode, pUser);
    terrno = TSDB_CODE_MND_NO_RIGHTS;
    return -1;
  }
  mndReleaseUser(pMnode, pUser);

  int32_t  cols = 0;
  SSchema *pSchema = pMeta->schema;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "connId");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_USER_LEN + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "user");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  // app name
  pShow->bytes[cols] = TSDB_APP_NAME_LEN + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "program");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  // app pid
  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "pid");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_IPv4ADDR_LEN + 6 + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "ip:port");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_TIMESTAMP;
  strcpy(pSchema[cols].name, "login_time");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_TIMESTAMP;
  strcpy(pSchema[cols].name, "last_access");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pMeta->numOfColumns = htons(cols);
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) {
    pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];
  }

  pShow->numOfRows = taosHashGetSize(pMgmt->cache->pHashTable);
  pShow->rowSize = pShow->offset[cols - 1] + pShow->bytes[cols - 1];

  return 0;
}

static int32_t mndRetrieveConns(SMnodeMsg *pMsg, SShowObj *pShow, char *data, int32_t rows) {
  SMnode   *pMnode = pMsg->pMnode;
  int32_t   numOfRows = 0;
  SConnObj *pConn = NULL;
  int32_t   cols = 0;
  char     *pWrite;
  char      ipStr[TSDB_IPv4ADDR_LEN + 6];

  while (numOfRows < rows) {
    pShow->pIter = mndGetNextConn(pMnode, pShow->pIter, &pConn);
    if (pConn == NULL) break;

    cols = 0;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int32_t *)pWrite = pConn->id;
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    STR_WITH_MAXSIZE_TO_VARSTR(pWrite, pConn->user, pShow->bytes[cols]);
    cols++;

    // app name
    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    STR_WITH_MAXSIZE_TO_VARSTR(pWrite, pConn->app, pShow->bytes[cols]);
    cols++;

    // app pid
    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int32_t *)pWrite = pConn->pid;
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    taosIpPort2String(pConn->ip, pConn->port, ipStr);
    STR_WITH_MAXSIZE_TO_VARSTR(pWrite, ipStr, pShow->bytes[cols]);
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int64_t *)pWrite = pConn->stime;
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    if (pConn->lastAccess < pConn->stime) pConn->lastAccess = pConn->stime;
    *(int64_t *)pWrite = pConn->lastAccess;
    cols++;

    numOfRows++;
  }

  pShow->numOfReads += numOfRows;

  return numOfRows;
}

static int32_t mndGetQueryMeta(SMnodeMsg *pMsg, SShowObj *pShow, STableMetaMsg *pMeta) {
  SMnode       *pMnode = pMsg->pMnode;
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;

  SUserObj *pUser = mndAcquireUser(pMnode, pMsg->user);
  if (pUser == NULL) return 0;
  if (!pUser->superAuth) {
    mndReleaseUser(pMnode, pUser);
    terrno = TSDB_CODE_MND_NO_RIGHTS;
    return -1;
  }
  mndReleaseUser(pMnode, pUser);

  int32_t  cols = 0;
  SSchema *pSchema = pMeta->schema;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "queryId");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "connId");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_USER_LEN + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "user");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_IPv4ADDR_LEN + 6 + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "ip:port");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 24;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "qid");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_TIMESTAMP;
  strcpy(pSchema[cols].name, "created_time");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_BIGINT;
  strcpy(pSchema[cols].name, "time");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = QUERY_OBJ_ID_SIZE + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "sql_obj_id");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "pid");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_EP_LEN + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "ep");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 1;
  pSchema[cols].type = TSDB_DATA_TYPE_BOOL;
  strcpy(pSchema[cols].name, "stable_query");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "sub_queries");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_SHOW_SUBQUERY_LEN + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "sub_query_info");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_SHOW_SQL_LEN + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "sql");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pMeta->numOfColumns = htons(cols);
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) {
    pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];
  }

  pShow->numOfRows = 1000000;
  pShow->rowSize = pShow->offset[cols - 1] + pShow->bytes[cols - 1];

  return 0;
}

static int32_t mndRetrieveQueries(SMnodeMsg *pMsg, SShowObj *pShow, char *data, int32_t rows) {
  SMnode   *pMnode = pMsg->pMnode;
  int32_t   numOfRows = 0;
  SConnObj *pConn = NULL;
  int32_t   cols = 0;
  char     *pWrite;
  void     *pIter;
  char      str[TSDB_IPv4ADDR_LEN + 6] = {0};

  while (numOfRows < rows) {
    pIter = mndGetNextConn(pMnode, pShow->pIter, &pConn);
    if (pConn == NULL) {
      pShow->pIter = pIter;
      break;
    }

    if (numOfRows + pConn->numOfQueries >= rows) {
      mndCancelGetNextConn(pMnode, pIter);
      break;
    }

    pShow->pIter = pIter;
    for (int32_t i = 0; i < pConn->numOfQueries; ++i) {
      SQueryDesc *pDesc = pConn->pQueries + i;
      cols = 0;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int64_t *)pWrite = htobe64(pDesc->queryId);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int64_t *)pWrite = htobe64(pConn->id);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      STR_WITH_MAXSIZE_TO_VARSTR(pWrite, pConn->user, pShow->bytes[cols]);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      snprintf(str, tListLen(str), "%s:%u", taosIpStr(pConn->ip), pConn->port);
      STR_WITH_MAXSIZE_TO_VARSTR(pWrite, str, pShow->bytes[cols]);
      cols++;

      char handleBuf[24] = {0};
      snprintf(handleBuf, tListLen(handleBuf), "%" PRIu64, htobe64(pDesc->qId));
      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;

      STR_WITH_MAXSIZE_TO_VARSTR(pWrite, handleBuf, pShow->bytes[cols]);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int64_t *)pWrite = htobe64(pDesc->stime);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int64_t *)pWrite = htobe64(pDesc->useconds);
      cols++;

      snprintf(str, tListLen(str), "0x%" PRIx64, htobe64(pDesc->sqlObjId));
      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      STR_WITH_MAXSIZE_TO_VARSTR(pWrite, str, pShow->bytes[cols]);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int32_t *)pWrite = htonl(pDesc->pid);
      cols++;

      char epBuf[TSDB_EP_LEN + 1] = {0};
      snprintf(epBuf, tListLen(epBuf), "%s:%u", pDesc->fqdn, pConn->port);
      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      STR_WITH_MAXSIZE_TO_VARSTR(pWrite, epBuf, pShow->bytes[cols]);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(bool *)pWrite = pDesc->stableQuery;
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int32_t *)pWrite = htonl(pDesc->numOfSub);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      STR_WITH_MAXSIZE_TO_VARSTR(pWrite, pDesc->subSqlInfo, pShow->bytes[cols]);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      STR_WITH_MAXSIZE_TO_VARSTR(pWrite, pDesc->sql, pShow->bytes[cols]);
      cols++;

      numOfRows++;
    }
  }

  pShow->numOfReads += numOfRows;
  return numOfRows;
}

static void mndCancelGetNextQuery(SMnode *pMnode, void *pIter) {
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;
  taosHashCancelIterate(pMgmt->cache->pHashTable, pIter);
}

static int32_t mndGetStreamMeta(SMnodeMsg *pMsg, SShowObj *pShow, STableMetaMsg *pMeta) {
  SMnode       *pMnode = pMsg->pMnode;
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;

  SUserObj *pUser = mndAcquireUser(pMnode, pMsg->user);
  if (pUser == NULL) return 0;
  if (!pUser->superAuth) {
    mndReleaseUser(pMnode, pUser);
    terrno = TSDB_CODE_MND_NO_RIGHTS;
    return -1;
  }
  mndReleaseUser(pMnode, pUser);

  int32_t  cols = 0;
  SSchema *pSchema = pMeta->schema;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "streamId");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "connId");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_USER_LEN + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "user");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_TABLE_NAME_LEN + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "dest table");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_IPv4ADDR_LEN + 6 + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "ip:port");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_TIMESTAMP;
  strcpy(pSchema[cols].name, "created time");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_TIMESTAMP;
  strcpy(pSchema[cols].name, "exec time");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_BIGINT;
  strcpy(pSchema[cols].name, "time(us)");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = TSDB_SHOW_SQL_LEN + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "sql");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "cycles");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pMeta->numOfColumns = htons(cols);
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) {
    pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];
  }

  pShow->numOfRows = 1000000;
  pShow->rowSize = pShow->offset[cols - 1] + pShow->bytes[cols - 1];

  return 0;
}

static int32_t mndRetrieveStreams(SMnodeMsg *pMsg, SShowObj *pShow, char *data, int32_t rows) {
  SMnode   *pMnode = pMsg->pMnode;
  int32_t   numOfRows = 0;
  SConnObj *pConn = NULL;
  int32_t   cols = 0;
  char     *pWrite;
  void     *pIter;
  char      ipStr[TSDB_IPv4ADDR_LEN + 6];

  while (numOfRows < rows) {
    pIter = mndGetNextConn(pMnode, pShow->pIter, &pConn);
    if (pConn == NULL) {
      pShow->pIter = pIter;
      break;
    }

    if (numOfRows + pConn->numOfStreams >= rows) {
      mndCancelGetNextConn(pMnode, pIter);
      break;
    }

    pShow->pIter = pIter;
    for (int32_t i = 0; i < pConn->numOfStreams; ++i) {
      SStreamDesc *pDesc = pConn->pStreams + i;
      cols = 0;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int64_t *)pWrite = htobe64(pDesc->streamId);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int64_t *)pWrite = htobe64(pConn->id);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      STR_WITH_MAXSIZE_TO_VARSTR(pWrite, pConn->user, pShow->bytes[cols]);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      STR_WITH_MAXSIZE_TO_VARSTR(pWrite, pDesc->dstTable, pShow->bytes[cols]);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      snprintf(ipStr, sizeof(ipStr), "%s:%u", taosIpStr(pConn->ip), pConn->port);
      STR_WITH_MAXSIZE_TO_VARSTR(pWrite, ipStr, pShow->bytes[cols]);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int64_t *)pWrite = htobe64(pDesc->ctime);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int64_t *)pWrite = htobe64(pDesc->stime);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int64_t *)pWrite = htobe64(pDesc->useconds);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      STR_WITH_MAXSIZE_TO_VARSTR(pWrite, pDesc->sql, pShow->bytes[cols]);
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int32_t *)pWrite = (int32_t)htobe64(pDesc->num);
      cols++;

      numOfRows++;
    }
  }

  pShow->numOfReads += numOfRows;
  return numOfRows;
}

static void mndCancelGetNextStream(SMnode *pMnode, void *pIter) {
  SProfileMgmt *pMgmt = &pMnode->profileMgmt;
  taosHashCancelIterate(pMgmt->cache->pHashTable, pIter);
}