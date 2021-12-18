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
#include "os.h"
#include "taoserror.h"
#include "tref.h"
#include "tfile.h"
#include "walInt.h"

static int walSeekFilePos(SWal* pWal, int64_t ver) {
  int code = 0;

  int64_t idxTfd = pWal->writeIdxTfd;
  int64_t logTfd = pWal->writeLogTfd;
  
  //seek position
  int64_t offset = (ver - walGetCurFileFirstVer(pWal)) * WAL_IDX_ENTRY_SIZE;
  code = tfLseek(idxTfd, offset, SEEK_SET);
  if(code != 0) {
    return -1;
  }
  int64_t readBuf[2];
  code = tfRead(idxTfd, readBuf, sizeof(readBuf));
  if(code != 0) {
    return -1;
  }
  //TODO:deserialize
  ASSERT(readBuf[0] == ver);
  code = tfLseek(logTfd, readBuf[1], SEEK_CUR);
  if (code != 0) {
    return -1;
  }
  return code;
}

int walChangeFileToLast(SWal *pWal) {
  int64_t idxTfd, logTfd;
  WalFileInfo* pRet = taosArrayGetLast(pWal->fileInfoSet);
  ASSERT(pRet != NULL);
  int64_t fileFirstVer = pRet->firstVer;

  char fnameStr[WAL_FILE_LEN];
  walBuildIdxName(pWal, fileFirstVer, fnameStr);
  idxTfd = tfOpenReadWrite(fnameStr);
  if(idxTfd < 0) {
    return -1;
  }
  walBuildLogName(pWal, fileFirstVer, fnameStr);
  logTfd = tfOpenReadWrite(fnameStr);
  if(logTfd < 0) {
    return -1;
  }
  //switch file
  pWal->writeIdxTfd = idxTfd;
  pWal->writeLogTfd = logTfd;
  //change status
  pWal->curStatus = WAL_CUR_FILE_WRITABLE;
  return 0;
}

int walChangeFile(SWal *pWal, int64_t ver) {
  int code = 0;
  int64_t idxTfd, logTfd;
  char fnameStr[WAL_FILE_LEN];
  code = tfClose(pWal->writeLogTfd);
  if(code != 0) {
   //TODO 
  }
  code = tfClose(pWal->writeIdxTfd);
  if(code != 0) {
   //TODO 
  }
  WalFileInfo tmpInfo;
  tmpInfo.firstVer = ver;
  //bsearch in fileSet
  WalFileInfo* pRet = taosArraySearch(pWal->fileInfoSet, &tmpInfo, compareWalFileInfo, TD_LE);
  ASSERT(pRet != NULL);
  int64_t fileFirstVer = pRet->firstVer;
  //closed
  if(taosArrayGetLast(pWal->fileInfoSet) != pRet) {
    pWal->curStatus &= ~WAL_CUR_FILE_WRITABLE;
    walBuildIdxName(pWal, fileFirstVer, fnameStr);
    idxTfd = tfOpenRead(fnameStr);
    walBuildLogName(pWal, fileFirstVer, fnameStr);
    logTfd = tfOpenRead(fnameStr);
  } else {
    pWal->curStatus |= WAL_CUR_FILE_WRITABLE;
    walBuildIdxName(pWal, fileFirstVer, fnameStr);
    idxTfd = tfOpenReadWrite(fnameStr);
    walBuildLogName(pWal, fileFirstVer, fnameStr);
    logTfd = tfOpenReadWrite(fnameStr);
  }

  pWal->writeLogTfd = logTfd;
  pWal->writeIdxTfd = idxTfd;
  return code;
}

int walGetVerOffset(SWal* pWal, int64_t ver) {
  int code;
  return 0;
}

int walSeekVer(SWal *pWal, int64_t ver) {
  int code;
  if(ver == pWal->lastVersion) {
    return 0;
  }
  if(ver > pWal->lastVersion || ver < pWal->firstVersion) {
    return -1;
  }
  if(ver < pWal->snapshotVersion) {
    //TODO: set flag to prevent roll back
  }
  if(ver < walGetCurFileFirstVer(pWal) || (ver > walGetCurFileLastVer(pWal))) {
    code = walChangeFile(pWal, ver);
    if(code != 0) {
      return -1;
    }
  }
  code = walSeekFilePos(pWal, ver);
  if(code != 0) {
    return -1;
  }
   
  return 0;
}