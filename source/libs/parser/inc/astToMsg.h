#ifndef TDENGINE_ASTTOMSG_H
#define TDENGINE_ASTTOMSG_H

#include "parserInt.h"
#include "taosmsg.h"

SCreateUserMsg* buildUserManipulationMsg(SSqlInfo* pInfo, int64_t id, char* msgBuf, int32_t msgLen);
SShowMsg* buildShowMsg(SShowInfo* pShowInfo, int64_t id, char* msgBuf, int32_t msgLen);
SCreateDbMsg* buildCreateDbMsg(SCreateDbInfo* pCreateDbInfo, char* msgBuf, int32_t msgLen);

#endif  // TDENGINE_ASTTOMSG_H