/*
 *  Copyright (c) 2019, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#if __ORDER_BIG_ENDIAN__
#define BYTE_ORDER_BIG_ENDIAN 1
#endif

#include "agent/otubus.hpp"

#include <mutex>

#include <sys/eventfd.h>

#include <openthread/commissioner.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>

#include "ncp_openthread.hpp"
#include "common/logging.hpp"

namespace otbr {
namespace ubus {

static UbusServer *sUbusServerInstance = NULL;
static int         sUbusEfd            = -1;
static void *      sJsonUri            = NULL;
static int         sBufNum;
static std::mutex *sNcpThreadMutex;

const static int PANID_LENGTH     = 10;
const static int XPANID_LENGTH    = 64;
const static int MASTERKEY_LENGTH = 64;

UbusServer::UbusServer(Ncp::ControllerOpenThread *aController)
    : mIfFinishScan(false)
    , mContext(nullptr)
    , mSockPath(nullptr)
    , mController(aController)
    , mSecond(0)
{
    memset(&mNetworkdataBuf, 0, sizeof(mNetworkdataBuf));
    memset(&mBuf, 0, sizeof(mBuf));

    blob_buf_init(&mBuf, 0);
    blob_buf_init(&mNetworkdataBuf, 0);
}

UbusServer &UbusServer::GetInstance(void)
{
    return *sUbusServerInstance;
}

void UbusServer::Initialize(Ncp::ControllerOpenThread *aController)
{
    sUbusServerInstance = new UbusServer(aController);
    otThreadSetReceiveDiagnosticGetCallback(aController->GetInstance(), &UbusServer::HandleDiagnosticGetResponse,
                                            sUbusServerInstance);
}

enum
{
    SETNETWORK,
    SET_NETWORK_MAX,
};

enum
{
    PSKD,
    EUI64,
    ADD_JOINER_MAX,
};

enum
{
    MASTERKEY,
    NETWORKNAME,
    EXTPANID,
    PANID,
    CHANNEL,
    PSKC,
    MGMTSET_MAX,
};

static const struct blobmsg_policy setNetworknamePolicy[SET_NETWORK_MAX] = {
    [SETNETWORK] = {.name = "networkname", .type = BLOBMSG_TYPE_STRING},
};

static const struct blobmsg_policy setPanIdPolicy[SET_NETWORK_MAX] = {
    [SETNETWORK] = {.name = "panid", .type = BLOBMSG_TYPE_STRING},
};

static const struct blobmsg_policy setExtPanIdPolicy[SET_NETWORK_MAX] = {
    [SETNETWORK] = {.name = "extpanid", .type = BLOBMSG_TYPE_STRING},
};

static const struct blobmsg_policy setChannelPolicy[SET_NETWORK_MAX] = {
    [SETNETWORK] = {.name = "channel", .type = BLOBMSG_TYPE_INT32},
};

static const struct blobmsg_policy setPskcPolicy[SET_NETWORK_MAX] = {
    [SETNETWORK] = {.name = "pskc", .type = BLOBMSG_TYPE_STRING},
};

static const struct blobmsg_policy setMasterkeyPolicy[SET_NETWORK_MAX] = {
    [SETNETWORK] = {.name = "masterkey", .type = BLOBMSG_TYPE_STRING},
};

static const struct blobmsg_policy setModePolicy[SET_NETWORK_MAX] = {
    [SETNETWORK] = {.name = "mode", .type = BLOBMSG_TYPE_STRING},
};

static const struct blobmsg_policy setLeaderPartitionIdPolicy[SET_NETWORK_MAX] = {
    [SETNETWORK] = {.name = "leaderpartitionid", .type = BLOBMSG_TYPE_INT32},
};

static const struct blobmsg_policy macfilterAddPolicy[SET_NETWORK_MAX] = {
    [SETNETWORK] = {.name = "addr", .type = BLOBMSG_TYPE_STRING},
};

static const struct blobmsg_policy macfilterRemovePolicy[SET_NETWORK_MAX] = {
    [SETNETWORK] = {.name = "addr", .type = BLOBMSG_TYPE_STRING},
};

static const struct blobmsg_policy macfilterSetStatePolicy[SET_NETWORK_MAX] = {
    [SETNETWORK] = {.name = "state", .type = BLOBMSG_TYPE_STRING},
};

static const struct blobmsg_policy removeJoinerPolicy[SET_NETWORK_MAX] = {
    [SETNETWORK] = {.name = "eui64", .type = BLOBMSG_TYPE_STRING},
};

static const struct blobmsg_policy addJoinerPolicy[ADD_JOINER_MAX] = {
    [PSKD]  = {.name = "pskd", .type = BLOBMSG_TYPE_STRING},
    [EUI64] = {.name = "eui64", .type = BLOBMSG_TYPE_STRING},
};

static const struct blobmsg_policy mgmtsetPolicy[MGMTSET_MAX] = {
    [MASTERKEY]   = {.name = "masterkey", .type = BLOBMSG_TYPE_STRING},
    [NETWORKNAME] = {.name = "networkname", .type = BLOBMSG_TYPE_STRING},
    [EXTPANID]    = {.name = "extpanid", .type = BLOBMSG_TYPE_STRING},
    [PANID]       = {.name = "panid", .type = BLOBMSG_TYPE_STRING},
    [CHANNEL]     = {.name = "channel", .type = BLOBMSG_TYPE_STRING},
    [PSKC]        = {.name = "pskc", .type = BLOBMSG_TYPE_STRING},
};

static const struct ubus_method otbrMethods[] = {
    {"scan", &UbusServer::UbusScanHandler, 0, 0, NULL, 0},
    {"channel", &UbusServer::UbusChannelHandler, 0, 0, NULL, 0},
    {"setchannel", &UbusServer::UbusSetChannelHandler, 0, 0, setChannelPolicy, ARRAY_SIZE(setChannelPolicy)},
    {"networkname", &UbusServer::UbusNetworknameHandler, 0, 0, NULL, 0},
    {"setnetworkname", &UbusServer::UbusSetNetworknameHandler, 0, 0, setNetworknamePolicy,
     ARRAY_SIZE(setNetworknamePolicy)},
    {"state", &UbusServer::UbusStateHandler, 0, 0, NULL, 0},
    {"panid", &UbusServer::UbusPanIdHandler, 0, 0, NULL, 0},
    {"setpanid", &UbusServer::UbusSetPanIdHandler, 0, 0, setPanIdPolicy, ARRAY_SIZE(setPanIdPolicy)},
    {"rloc16", &UbusServer::UbusRloc16Handler, 0, 0, NULL, 0},
    {"extpanid", &UbusServer::UbusExtPanIdHandler, 0, 0, NULL, 0},
    {"setextpanid", &UbusServer::UbusSetExtPanIdHandler, 0, 0, setExtPanIdPolicy, ARRAY_SIZE(setExtPanIdPolicy)},
    {"masterkey", &UbusServer::UbusMasterkeyHandler, 0, 0, NULL, 0},
    {"setmasterkey", &UbusServer::UbusSetMasterkeyHandler, 0, 0, setMasterkeyPolicy, ARRAY_SIZE(setMasterkeyPolicy)},
    {"pskc", &UbusServer::UbusPskcHandler, 0, 0, NULL, 0},
    {"setpskc", &UbusServer::UbusSetPskcHandler, 0, 0, setPskcPolicy, ARRAY_SIZE(setPskcPolicy)},
    {"threadstart", &UbusServer::UbusThreadStartHandler, 0, 0, NULL, 0},
    {"threadstop", &UbusServer::UbusThreadStopHandler, 0, 0, NULL, 0},
    {"neighbor", &UbusServer::UbusNeighborHandler, 0, 0, NULL, 0},
    {"parent", &UbusServer::UbusParentHandler, 0, 0, NULL, 0},
    {"mode", &UbusServer::UbusModeHandler, 0, 0, NULL, 0},
    {"setmode", &UbusServer::UbusSetModeHandler, 0, 0, setModePolicy, ARRAY_SIZE(setModePolicy)},
    {"leaderpartitionid", &UbusServer::UbusLeaderPartitionIdHandler, 0, 0, NULL, 0},
    {"setleaderpartitionid", &UbusServer::UbusSetLeaderPartitionIdHandler, 0, 0, setLeaderPartitionIdPolicy,
     ARRAY_SIZE(setLeaderPartitionIdPolicy)},
    {"leave", &UbusServer::UbusLeaveHandler, 0, 0, NULL, 0},
    {"leaderdata", &UbusServer::UbusLeaderdataHandler, 0, 0, NULL, 0},
    {"networkdata", &UbusServer::UbusNetworkdataHandler, 0, 0, NULL, 0},
    {"commissionerstart", &UbusServer::UbusCommissionerStartHandler, 0, 0, NULL, 0},
    {"joinernum", &UbusServer::UbusJoinerNumHandler, 0, 0, NULL, 0},
    {"joinerremove", &UbusServer::UbusJoinerRemoveHandler, 0, 0, NULL, 0},
    {"macfiltersetstate", &UbusServer::UbusMacfilterSetStateHandler, 0, 0, macfilterSetStatePolicy,
     ARRAY_SIZE(macfilterSetStatePolicy)},
    {"macfilteradd", &UbusServer::UbusMacfilterAddHandler, 0, 0, macfilterAddPolicy, ARRAY_SIZE(macfilterAddPolicy)},
    {"macfilterremove", &UbusServer::UbusMacfilterRemoveHandler, 0, 0, macfilterRemovePolicy,
     ARRAY_SIZE(macfilterRemovePolicy)},
    {"macfilterclear", &UbusServer::UbusMacfilterClearHandler, 0, 0, NULL, 0},
    {"macfilterstate", &UbusServer::UbusMacfilterStateHandler, 0, 0, NULL, 0},
    {"macfilteraddr", &UbusServer::UbusMacfilterAddrHandler, 0, 0, NULL, 0},
    {"joineradd", &UbusServer::UbusJoinerAddHandler, 0, 0, addJoinerPolicy, ARRAY_SIZE(addJoinerPolicy)},
    {"mgmtset", &UbusServer::UbusMgmtsetHandler, 0, 0, mgmtsetPolicy, ARRAY_SIZE(mgmtsetPolicy)},
};

static struct ubus_object_type otbrObjType = {"otbr_prog", 0, otbrMethods, ARRAY_SIZE(otbrMethods)};

static struct ubus_object otbr = {
    avl : {},
    name : "otbr",
    id : 0,
    path : NULL,
    type : &otbrObjType,
    subscribe_cb : NULL,
    has_subscribers : false,
    methods : otbrMethods,
    n_methods : ARRAY_SIZE(otbrMethods),
};

void UbusServer::ProcessScan(void)
{
    otError  error        = OT_ERROR_NONE;
    uint32_t scanChannels = 0;
    uint16_t scanDuration = 0;

    sNcpThreadMutex->lock();
    SuccessOrExit(error = otLinkActiveScan(mController->GetInstance(), scanChannels, scanDuration,
                                           &UbusServer::HandleActiveScanResult, this));
exit:
    sNcpThreadMutex->unlock();
    return;
}

void UbusServer::HandleActiveScanResult(otActiveScanResult *aResult, void *aContext)
{
    static_cast<UbusServer *>(aContext)->HandleActiveScanResultDetail(aResult);
}

void UbusServer::OutputBytes(const uint8_t *aBytes, uint8_t aLength, char *aOutput)
{
    char byte2char[5] = "";
    for (int i = 0; i < aLength; i++)
    {
        sprintf(byte2char, "%02x", aBytes[i]);
        strcat(aOutput, byte2char);
    }
}

void UbusServer::AppendResult(otError aError, struct ubus_context *aContext, struct ubus_request_data *aRequest)
{
    blobmsg_add_u16(&mBuf, "Error", aError);
    ubus_send_reply(aContext, aRequest, mBuf.head);
}

void UbusServer::HandleActiveScanResultDetail(otActiveScanResult *aResult)
{
    void *jsonList = NULL;

    char panidstring[PANID_LENGTH];
    char xpanidstring[XPANID_LENGTH] = "";

    if (aResult == NULL)
    {
        blobmsg_close_array(&mBuf, sJsonUri);
        mIfFinishScan = true;
        goto exit;
    }

    jsonList = blobmsg_open_table(&mBuf, NULL);

    blobmsg_add_u32(&mBuf, "IsJoinable", aResult->mIsJoinable);

    blobmsg_add_string(&mBuf, "NetworkName", aResult->mNetworkName.m8);

    OutputBytes(aResult->mExtendedPanId.m8, OT_EXT_PAN_ID_SIZE, xpanidstring);
    blobmsg_add_string(&mBuf, "ExtendedPanId", xpanidstring);

    sprintf(panidstring, "0x%04x", aResult->mPanId);
    blobmsg_add_string(&mBuf, "PanId", panidstring);

    blobmsg_add_u32(&mBuf, "Channel", aResult->mChannel);

    blobmsg_add_u32(&mBuf, "Rssi", aResult->mRssi);

    blobmsg_add_u32(&mBuf, "Lqi", aResult->mLqi);

    blobmsg_close_table(&mBuf, jsonList);

exit:
    return;
}

int UbusServer::UbusScanHandler(struct ubus_context *     aContext,
                                struct ubus_object *      aObj,
                                struct ubus_request_data *aRequest,
                                const char *              aMethod,
                                struct blob_attr *        aMsg)
{
    return GetInstance().UbusScanHandlerDetail(aContext, aObj, aRequest, aMethod, aMsg);
}

int UbusServer::UbusScanHandlerDetail(struct ubus_context *     aContext,
                                      struct ubus_object *      aObj,
                                      struct ubus_request_data *aRequest,
                                      const char *              aMethod,
                                      struct blob_attr *        aMsg)
{
    OT_UNUSED_VARIABLE(aObj);
    OT_UNUSED_VARIABLE(aMethod);
    OT_UNUSED_VARIABLE(aMsg);

    otError  error = OT_ERROR_NONE;
    uint64_t eventNum;
    ssize_t  retval;

    blob_buf_init(&mBuf, 0);
    sJsonUri = blobmsg_open_array(&mBuf, "scan_list");

    mIfFinishScan = 0;
    sUbusServerInstance->ProcessScan();

    eventNum = 1;
    retval   = write(sUbusEfd, &eventNum, sizeof(uint64_t));
    if (retval != sizeof(uint64_t))
    {
        error = OT_ERROR_FAILED;
        goto exit;
    }

    while (!mIfFinishScan)
    {
        sleep(1);
    }

exit:
    AppendResult(error, aContext, aRequest);
    return 0;
}

int UbusServer::UbusChannelHandler(struct ubus_context *     aContext,
                                   struct ubus_object *      aObj,
                                   struct ubus_request_data *aRequest,
                                   const char *              aMethod,
                                   struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "channel");
}

int UbusServer::UbusSetChannelHandler(struct ubus_context *     aContext,
                                      struct ubus_object *      aObj,
                                      struct ubus_request_data *aRequest,
                                      const char *              aMethod,
                                      struct blob_attr *        aMsg)
{
    return GetInstance().UbusSetInformation(aContext, aObj, aRequest, aMethod, aMsg, "channel");
}

int UbusServer::UbusJoinerNumHandler(struct ubus_context *     aContext,
                                     struct ubus_object *      aObj,
                                     struct ubus_request_data *aRequest,
                                     const char *              aMethod,
                                     struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "joinernum");
}

int UbusServer::UbusNetworknameHandler(struct ubus_context *     aContext,
                                       struct ubus_object *      aObj,
                                       struct ubus_request_data *aRequest,
                                       const char *              aMethod,
                                       struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "networkname");
}

int UbusServer::UbusSetNetworknameHandler(struct ubus_context *     aContext,
                                          struct ubus_object *      aObj,
                                          struct ubus_request_data *aRequest,
                                          const char *              aMethod,
                                          struct blob_attr *        aMsg)
{
    return GetInstance().UbusSetInformation(aContext, aObj, aRequest, aMethod, aMsg, "networkname");
}

int UbusServer::UbusStateHandler(struct ubus_context *     aContext,
                                 struct ubus_object *      aObj,
                                 struct ubus_request_data *aRequest,
                                 const char *              aMethod,
                                 struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "state");
}

int UbusServer::UbusRloc16Handler(struct ubus_context *     aContext,
                                  struct ubus_object *      aObj,
                                  struct ubus_request_data *aRequest,
                                  const char *              aMethod,
                                  struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "rloc16");
}

int UbusServer::UbusPanIdHandler(struct ubus_context *     aContext,
                                 struct ubus_object *      aObj,
                                 struct ubus_request_data *aRequest,
                                 const char *              aMethod,
                                 struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "panid");
}

int UbusServer::UbusSetPanIdHandler(struct ubus_context *     aContext,
                                    struct ubus_object *      aObj,
                                    struct ubus_request_data *aRequest,
                                    const char *              aMethod,
                                    struct blob_attr *        aMsg)
{
    return GetInstance().UbusSetInformation(aContext, aObj, aRequest, aMethod, aMsg, "panid");
}

int UbusServer::UbusExtPanIdHandler(struct ubus_context *     aContext,
                                    struct ubus_object *      aObj,
                                    struct ubus_request_data *aRequest,
                                    const char *              aMethod,
                                    struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "extpanid");
}

int UbusServer::UbusSetExtPanIdHandler(struct ubus_context *     aContext,
                                       struct ubus_object *      aObj,
                                       struct ubus_request_data *aRequest,
                                       const char *              aMethod,
                                       struct blob_attr *        aMsg)
{
    return GetInstance().UbusSetInformation(aContext, aObj, aRequest, aMethod, aMsg, "extpanid");
}

int UbusServer::UbusPskcHandler(struct ubus_context *     aContext,
                                struct ubus_object *      aObj,
                                struct ubus_request_data *aRequest,
                                const char *              aMethod,
                                struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "pskc");
}

int UbusServer::UbusSetPskcHandler(struct ubus_context *     aContext,
                                   struct ubus_object *      aObj,
                                   struct ubus_request_data *aRequest,
                                   const char *              aMethod,
                                   struct blob_attr *        aMsg)
{
    return GetInstance().UbusSetInformation(aContext, aObj, aRequest, aMethod, aMsg, "pskc");
}

int UbusServer::UbusMasterkeyHandler(struct ubus_context *     aContext,
                                     struct ubus_object *      aObj,
                                     struct ubus_request_data *aRequest,
                                     const char *              aMethod,
                                     struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "masterkey");
}

int UbusServer::UbusSetMasterkeyHandler(struct ubus_context *     aContext,
                                        struct ubus_object *      aObj,
                                        struct ubus_request_data *aRequest,
                                        const char *              aMethod,
                                        struct blob_attr *        aMsg)
{
    return GetInstance().UbusSetInformation(aContext, aObj, aRequest, aMethod, aMsg, "masterkey");
}

int UbusServer::UbusThreadStartHandler(struct ubus_context *     aContext,
                                       struct ubus_object *      aObj,
                                       struct ubus_request_data *aRequest,
                                       const char *              aMethod,
                                       struct blob_attr *        aMsg)
{
    return GetInstance().UbusThreadHandler(aContext, aObj, aRequest, aMethod, aMsg, "start");
}

int UbusServer::UbusThreadStopHandler(struct ubus_context *     aContext,
                                      struct ubus_object *      aObj,
                                      struct ubus_request_data *aRequest,
                                      const char *              aMethod,
                                      struct blob_attr *        aMsg)
{
    return GetInstance().UbusThreadHandler(aContext, aObj, aRequest, aMethod, aMsg, "stop");
}

int UbusServer::UbusParentHandler(struct ubus_context *     aContext,
                                  struct ubus_object *      aObj,
                                  struct ubus_request_data *aRequest,
                                  const char *              aMethod,
                                  struct blob_attr *        aMsg)
{
    return GetInstance().UbusParentHandlerDetail(aContext, aObj, aRequest, aMethod, aMsg);
}

int UbusServer::UbusNeighborHandler(struct ubus_context *     aContext,
                                    struct ubus_object *      aObj,
                                    struct ubus_request_data *aRequest,
                                    const char *              aMethod,
                                    struct blob_attr *        aMsg)
{
    return GetInstance().UbusNeighborHandlerDetail(aContext, aObj, aRequest, aMethod, aMsg);
}

int UbusServer::UbusModeHandler(struct ubus_context *     aContext,
                                struct ubus_object *      aObj,
                                struct ubus_request_data *aRequest,
                                const char *              aMethod,
                                struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "mode");
}

int UbusServer::UbusSetModeHandler(struct ubus_context *     aContext,
                                   struct ubus_object *      aObj,
                                   struct ubus_request_data *aRequest,
                                   const char *              aMethod,
                                   struct blob_attr *        aMsg)
{
    return GetInstance().UbusSetInformation(aContext, aObj, aRequest, aMethod, aMsg, "mode");
}

int UbusServer::UbusLeaderPartitionIdHandler(struct ubus_context *     aContext,
                                             struct ubus_object *      aObj,
                                             struct ubus_request_data *aRequest,
                                             const char *              aMethod,
                                             struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "leaderpartitionid");
}

int UbusServer::UbusSetLeaderPartitionIdHandler(struct ubus_context *     aContext,
                                                struct ubus_object *      aObj,
                                                struct ubus_request_data *aRequest,
                                                const char *              aMethod,
                                                struct blob_attr *        aMsg)
{
    return GetInstance().UbusSetInformation(aContext, aObj, aRequest, aMethod, aMsg, "leaderpartitionid");
}

int UbusServer::UbusLeaveHandler(struct ubus_context *     aContext,
                                 struct ubus_object *      aObj,
                                 struct ubus_request_data *aRequest,
                                 const char *              aMethod,
                                 struct blob_attr *        aMsg)
{
    return GetInstance().UbusLeaveHandlerDetail(aContext, aObj, aRequest, aMethod, aMsg);
}

int UbusServer::UbusLeaderdataHandler(struct ubus_context *     aContext,
                                      struct ubus_object *      aObj,
                                      struct ubus_request_data *aRequest,
                                      const char *              aMethod,
                                      struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "leaderdata");
}

int UbusServer::UbusNetworkdataHandler(struct ubus_context *     aContext,
                                       struct ubus_object *      aObj,
                                       struct ubus_request_data *aRequest,
                                       const char *              aMethod,
                                       struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "networkdata");
}

int UbusServer::UbusCommissionerStartHandler(struct ubus_context *     aContext,
                                             struct ubus_object *      aObj,
                                             struct ubus_request_data *aRequest,
                                             const char *              aMethod,
                                             struct blob_attr *        aMsg)
{
    return GetInstance().UbusCommissioner(aContext, aObj, aRequest, aMethod, aMsg, "start");
}

int UbusServer::UbusJoinerRemoveHandler(struct ubus_context *     aContext,
                                        struct ubus_object *      aObj,
                                        struct ubus_request_data *aRequest,
                                        const char *              aMethod,
                                        struct blob_attr *        aMsg)
{
    return GetInstance().UbusCommissioner(aContext, aObj, aRequest, aMethod, aMsg, "joinerremove");
}

int UbusServer::UbusMgmtsetHandler(struct ubus_context *     aContext,
                                   struct ubus_object *      aObj,
                                   struct ubus_request_data *aRequest,
                                   const char *              aMethod,
                                   struct blob_attr *        aMsg)
{
    return GetInstance().UbusMgmtset(aContext, aObj, aRequest, aMethod, aMsg);
}

int UbusServer::UbusJoinerAddHandler(struct ubus_context *     aContext,
                                     struct ubus_object *      aObj,
                                     struct ubus_request_data *aRequest,
                                     const char *              aMethod,
                                     struct blob_attr *        aMsg)
{
    return GetInstance().UbusCommissioner(aContext, aObj, aRequest, aMethod, aMsg, "joineradd");
}

int UbusServer::UbusMacfilterAddrHandler(struct ubus_context *     aContext,
                                         struct ubus_object *      aObj,
                                         struct ubus_request_data *aRequest,
                                         const char *              aMethod,
                                         struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "macfilteraddr");
}

int UbusServer::UbusMacfilterStateHandler(struct ubus_context *     aContext,
                                          struct ubus_object *      aObj,
                                          struct ubus_request_data *aRequest,
                                          const char *              aMethod,
                                          struct blob_attr *        aMsg)
{
    return GetInstance().UbusGetInformation(aContext, aObj, aRequest, aMethod, aMsg, "macfilterstate");
}

int UbusServer::UbusMacfilterAddHandler(struct ubus_context *     aContext,
                                        struct ubus_object *      aObj,
                                        struct ubus_request_data *aRequest,
                                        const char *              aMethod,
                                        struct blob_attr *        aMsg)
{
    return GetInstance().UbusSetInformation(aContext, aObj, aRequest, aMethod, aMsg, "macfilteradd");
}

int UbusServer::UbusMacfilterRemoveHandler(struct ubus_context *     aContext,
                                           struct ubus_object *      aObj,
                                           struct ubus_request_data *aRequest,
                                           const char *              aMethod,
                                           struct blob_attr *        aMsg)
{
    return GetInstance().UbusSetInformation(aContext, aObj, aRequest, aMethod, aMsg, "macfilterremove");
}

int UbusServer::UbusMacfilterSetStateHandler(struct ubus_context *     aContext,
                                             struct ubus_object *      aObj,
                                             struct ubus_request_data *aRequest,
                                             const char *              aMethod,
                                             struct blob_attr *        aMsg)
{
    return GetInstance().UbusSetInformation(aContext, aObj, aRequest, aMethod, aMsg, "macfiltersetstate");
}

int UbusServer::UbusMacfilterClearHandler(struct ubus_context *     aContext,
                                          struct ubus_object *      aObj,
                                          struct ubus_request_data *aRequest,
                                          const char *              aMethod,
                                          struct blob_attr *        aMsg)
{
    return GetInstance().UbusSetInformation(aContext, aObj, aRequest, aMethod, aMsg, "macfilterclear");
}

int UbusServer::UbusLeaveHandlerDetail(struct ubus_context *     aContext,
                                       struct ubus_object *      aObj,
                                       struct ubus_request_data *aRequest,
                                       const char *              aMethod,
                                       struct blob_attr *        aMsg)
{
    OT_UNUSED_VARIABLE(aObj);
    OT_UNUSED_VARIABLE(aMethod);
    OT_UNUSED_VARIABLE(aMsg);

    otError  error = OT_ERROR_NONE;
    uint64_t eventNum;
    ssize_t  retval;

    sNcpThreadMutex->lock();
    otInstanceFactoryReset(mController->GetInstance());

    eventNum = 1;
    retval   = write(sUbusEfd, &eventNum, sizeof(uint64_t));
    if (retval != sizeof(uint64_t))
    {
        error = OT_ERROR_FAILED;
        goto exit;
    }

    blob_buf_init(&mBuf, 0);

exit:
    sNcpThreadMutex->unlock();
    AppendResult(error, aContext, aRequest);
    return 0;
}
int UbusServer::UbusThreadHandler(struct ubus_context *     aContext,
                                  struct ubus_object *      aObj,
                                  struct ubus_request_data *aRequest,
                                  const char *              aMethod,
                                  struct blob_attr *        aMsg,
                                  const char *              aAction)
{
    OT_UNUSED_VARIABLE(aObj);
    OT_UNUSED_VARIABLE(aMethod);
    OT_UNUSED_VARIABLE(aMsg);

    otError error = OT_ERROR_NONE;

    blob_buf_init(&mBuf, 0);

    if (!strcmp(aAction, "start"))
    {
        sNcpThreadMutex->lock();
        SuccessOrExit(error = otIp6SetEnabled(mController->GetInstance(), true));
        SuccessOrExit(error = otThreadSetEnabled(mController->GetInstance(), true));
    }
    else if (!strcmp(aAction, "stop"))
    {
        sNcpThreadMutex->lock();
        SuccessOrExit(error = otThreadSetEnabled(mController->GetInstance(), false));
        SuccessOrExit(error = otIp6SetEnabled(mController->GetInstance(), false));
    }

exit:
    sNcpThreadMutex->unlock();
    AppendResult(error, aContext, aRequest);
    return 0;
}

int UbusServer::UbusParentHandlerDetail(struct ubus_context *     aContext,
                                        struct ubus_object *      aObj,
                                        struct ubus_request_data *aRequest,
                                        const char *              aMethod,
                                        struct blob_attr *        aMsg)
{
    OT_UNUSED_VARIABLE(aObj);
    OT_UNUSED_VARIABLE(aMethod);
    OT_UNUSED_VARIABLE(aMsg);

    otError      error = OT_ERROR_NONE;
    otRouterInfo parentInfo;
    char         extAddress[XPANID_LENGTH] = "";
    char         transfer[XPANID_LENGTH]   = "";
    void *       jsonList                  = NULL;
    void *       jsonArray                 = NULL;

    blob_buf_init(&mBuf, 0);

    sNcpThreadMutex->lock();
    SuccessOrExit(error = otThreadGetParentInfo(mController->GetInstance(), &parentInfo));

    jsonArray = blobmsg_open_array(&mBuf, "parent_list");
    jsonList  = blobmsg_open_table(&mBuf, "parent");
    blobmsg_add_string(&mBuf, "Role", "R");

    sprintf(transfer, "0x%04x", parentInfo.mRloc16);
    blobmsg_add_string(&mBuf, "Rloc16", transfer);

    sprintf(transfer, "%3d", parentInfo.mAge);
    blobmsg_add_string(&mBuf, "Age", transfer);

    OutputBytes(parentInfo.mExtAddress.m8, sizeof(parentInfo.mExtAddress.m8), extAddress);
    blobmsg_add_string(&mBuf, "ExtAddress", extAddress);

    blobmsg_add_u16(&mBuf, "LinkQualityIn", parentInfo.mLinkQualityIn);

    blobmsg_close_table(&mBuf, jsonList);
    blobmsg_close_array(&mBuf, jsonArray);

exit:
    sNcpThreadMutex->unlock();
    AppendResult(error, aContext, aRequest);
    return error;
}

int UbusServer::UbusNeighborHandlerDetail(struct ubus_context *     aContext,
                                          struct ubus_object *      aObj,
                                          struct ubus_request_data *aRequest,
                                          const char *              aMethod,
                                          struct blob_attr *        aMsg)
{
    OT_UNUSED_VARIABLE(aObj);
    OT_UNUSED_VARIABLE(aMethod);
    OT_UNUSED_VARIABLE(aMsg);

    otError                error = OT_ERROR_NONE;
    otNeighborInfo         neighborInfo;
    otNeighborInfoIterator iterator                  = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    char                   transfer[XPANID_LENGTH]   = "";
    void *                 jsonList                  = NULL;
    char                   mode[5]                   = "";
    char                   extAddress[XPANID_LENGTH] = "";

    blob_buf_init(&mBuf, 0);

    sJsonUri = blobmsg_open_array(&mBuf, "neighbor_list");

    sNcpThreadMutex->lock();
    while (otThreadGetNextNeighborInfo(mController->GetInstance(), &iterator, &neighborInfo) == OT_ERROR_NONE)
    {
        jsonList = blobmsg_open_table(&mBuf, NULL);

        blobmsg_add_string(&mBuf, "Role", neighborInfo.mIsChild ? "C" : "R");

        sprintf(transfer, "0x%04x", neighborInfo.mRloc16);
        blobmsg_add_string(&mBuf, "Rloc16", transfer);

        sprintf(transfer, "%3d", neighborInfo.mAge);
        blobmsg_add_string(&mBuf, "Age", transfer);

        sprintf(transfer, "%8d", neighborInfo.mAverageRssi);
        blobmsg_add_string(&mBuf, "AvgRssi", transfer);

        sprintf(transfer, "%9d", neighborInfo.mLastRssi);
        blobmsg_add_string(&mBuf, "LastRssi", transfer);

        if (neighborInfo.mRxOnWhenIdle)
        {
            strcat(mode, "r");
        }

        if (neighborInfo.mSecureDataRequest)
        {
            strcat(mode, "s");
        }

        if (neighborInfo.mFullThreadDevice)
        {
            strcat(mode, "d");
        }

        if (neighborInfo.mFullNetworkData)
        {
            strcat(mode, "n");
        }
        blobmsg_add_string(&mBuf, "Mode", mode);

        OutputBytes(neighborInfo.mExtAddress.m8, sizeof(neighborInfo.mExtAddress.m8), extAddress);
        blobmsg_add_string(&mBuf, "ExtAddress", extAddress);

        blobmsg_add_u16(&mBuf, "LinkQualityIn", neighborInfo.mLinkQualityIn);

        blobmsg_close_table(&mBuf, jsonList);

        memset(mode, 0, sizeof(mode));
        memset(extAddress, 0, sizeof(extAddress));
    }

    blobmsg_close_array(&mBuf, sJsonUri);

    sNcpThreadMutex->unlock();

    AppendResult(error, aContext, aRequest);
    return 0;
}

int UbusServer::UbusMgmtset(struct ubus_context *     aContext,
                            struct ubus_object *      aObj,
                            struct ubus_request_data *aRequest,
                            const char *              aMethod,
                            struct blob_attr *        aMsg)
{
    OT_UNUSED_VARIABLE(aObj);
    OT_UNUSED_VARIABLE(aMethod);
    OT_UNUSED_VARIABLE(aMsg);

    otError              error = OT_ERROR_NONE;
    struct blob_attr *   tb[MGMTSET_MAX];
    otOperationalDataset dataset;
    uint8_t              tlvs[128];
    long                 value;
    int                  length = 0;

    SuccessOrExit(error = otDatasetGetActive(mController->GetInstance(), &dataset));

    blobmsg_parse(mgmtsetPolicy, MGMTSET_MAX, tb, blob_data(aMsg), blob_len(aMsg));
    if (tb[MASTERKEY] != NULL)
    {
        dataset.mComponents.mIsMasterKeyPresent = true;
        VerifyOrExit((length = Hex2Bin(blobmsg_get_string(tb[MASTERKEY]), dataset.mMasterKey.m8,
                                       sizeof(dataset.mMasterKey.m8))) == OT_MASTER_KEY_SIZE,
                     error = OT_ERROR_PARSE);
        length = 0;
    }
    if (tb[NETWORKNAME] != NULL)
    {
        dataset.mComponents.mIsNetworkNamePresent = true;
        VerifyOrExit((length = static_cast<int>(strlen(blobmsg_get_string(tb[NETWORKNAME])))) <=
                         OT_NETWORK_NAME_MAX_SIZE,
                     error = OT_ERROR_PARSE);
        memset(&dataset.mNetworkName, 0, sizeof(dataset.mNetworkName));
        memcpy(dataset.mNetworkName.m8, blobmsg_get_string(tb[NETWORKNAME]), static_cast<size_t>(length));
        length = 0;
    }
    if (tb[EXTPANID] != NULL)
    {
        dataset.mComponents.mIsExtendedPanIdPresent = true;
        VerifyOrExit(Hex2Bin(blobmsg_get_string(tb[EXTPANID]), dataset.mExtendedPanId.m8,
                             sizeof(dataset.mExtendedPanId.m8)) >= 0,
                     error = OT_ERROR_PARSE);
    }
    if (tb[PANID] != NULL)
    {
        dataset.mComponents.mIsPanIdPresent = true;
        SuccessOrExit(error = ParseLong(blobmsg_get_string(tb[PANID]), value));
        dataset.mPanId = static_cast<otPanId>(value);
    }
    if (tb[CHANNEL] != NULL)
    {
        dataset.mComponents.mIsChannelPresent = true;
        SuccessOrExit(error = ParseLong(blobmsg_get_string(tb[CHANNEL]), value));
        dataset.mChannel = static_cast<uint16_t>(value);
    }
    if (tb[PSKC] != NULL)
    {
        dataset.mComponents.mIsPskcPresent = true;
        VerifyOrExit((length = Hex2Bin(blobmsg_get_string(tb[PSKC]), dataset.mPskc.m8, sizeof(dataset.mPskc.m8))) ==
                         OT_PSKC_MAX_SIZE,
                     error = OT_ERROR_PARSE);
        length = 0;
    }
    dataset.mActiveTimestamp++;
    if (otCommissionerGetState(mController->GetInstance()) == OT_COMMISSIONER_STATE_DISABLED)
    {
        otCommissionerStop(mController->GetInstance());
    }
    SuccessOrExit(
        error = otDatasetSendMgmtActiveSet(mController->GetInstance(), &dataset, tlvs, static_cast<uint8_t>(length)));
exit:
    AppendResult(error, aContext, aRequest);
    return 0;
}

int UbusServer::UbusCommissioner(struct ubus_context *     aContext,
                                 struct ubus_object *      aObj,
                                 struct ubus_request_data *aRequest,
                                 const char *              aMethod,
                                 struct blob_attr *        aMsg,
                                 const char *              aAction)
{
    OT_UNUSED_VARIABLE(aObj);
    OT_UNUSED_VARIABLE(aMethod);
    OT_UNUSED_VARIABLE(aMsg);

    otError error = OT_ERROR_NONE;

    sNcpThreadMutex->lock();

    if (!strcmp(aAction, "start"))
    {
        if (otCommissionerGetState(mController->GetInstance()) == OT_COMMISSIONER_STATE_DISABLED)
        {
            error = otCommissionerStart(mController->GetInstance(), &UbusServer::HandleStateChanged,
                                        &UbusServer::HandleJoinerEvent, this);
        }
    }
    else if (!strcmp(aAction, "joineradd"))
    {
        struct blob_attr *  tb[ADD_JOINER_MAX];
        otExtAddress        addr;
        const otExtAddress *addrPtr = NULL;
        char *              pskd    = NULL;

        blobmsg_parse(addJoinerPolicy, ADD_JOINER_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[PSKD] != NULL)
        {
            pskd = blobmsg_get_string(tb[PSKD]);
        }
        if (tb[EUI64] != NULL)
        {
            if (!strcmp(blobmsg_get_string(tb[EUI64]), "*"))
            {
                addrPtr = NULL;
                memset(&addr, 0, sizeof(addr));
            }
            else
            {
                VerifyOrExit(Hex2Bin(blobmsg_get_string(tb[EUI64]), addr.m8, sizeof(addr)) == sizeof(addr),
                             error = OT_ERROR_PARSE);
                addrPtr = &addr;
            }
        }

        unsigned long timeout = kDefaultJoinerTimeout;
        SuccessOrExit(
            error = otCommissionerAddJoiner(mController->GetInstance(), addrPtr, pskd, static_cast<uint32_t>(timeout)));
    }
    else if (!strcmp(aAction, "joinerremove"))
    {
        struct blob_attr *  tb[SET_NETWORK_MAX];
        otExtAddress        addr;
        const otExtAddress *addrPtr = NULL;

        blobmsg_parse(removeJoinerPolicy, SET_NETWORK_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[SETNETWORK] != NULL)
        {
            if (strcmp(blobmsg_get_string(tb[SETNETWORK]), "*") == 0)
            {
                addrPtr = NULL;
            }
            else
            {
                VerifyOrExit(Hex2Bin(blobmsg_get_string(tb[SETNETWORK]), addr.m8, sizeof(addr)) == sizeof(addr),
                             error = OT_ERROR_PARSE);
                addrPtr = &addr;
            }
        }

        SuccessOrExit(error = otCommissionerRemoveJoiner(mController->GetInstance(), addrPtr));
    }

exit:
    sNcpThreadMutex->unlock();
    blob_buf_init(&mBuf, 0);
    AppendResult(error, aContext, aRequest);
    return 0;
}

void UbusServer::HandleStateChanged(otCommissionerState aState, void *aContext)
{
    static_cast<UbusServer *>(aContext)->HandleStateChanged(aState);
}

void UbusServer::HandleStateChanged(otCommissionerState aState)
{
    switch (aState)
    {
    case OT_COMMISSIONER_STATE_DISABLED:
        otbrLog(OTBR_LOG_INFO, "commissioner state disabled");
        break;
    case OT_COMMISSIONER_STATE_ACTIVE:
        otbrLog(OTBR_LOG_INFO, "commissioner state active");
        break;
    case OT_COMMISSIONER_STATE_PETITION:
        otbrLog(OTBR_LOG_INFO, "commissioner state petition");
        break;
    }
}

void UbusServer::HandleJoinerEvent(otCommissionerJoinerEvent aEvent, const otExtAddress *aJoinerId, void *aContext)
{
    static_cast<UbusServer *>(aContext)->HandleJoinerEvent(aEvent, aJoinerId);
}

void UbusServer::HandleJoinerEvent(otCommissionerJoinerEvent aEvent, const otExtAddress *aJoinerId)
{
    OT_UNUSED_VARIABLE(aJoinerId);

    switch (aEvent)
    {
    case OT_COMMISSIONER_JOINER_START:
        otbrLog(OTBR_LOG_INFO, "joiner start");
        break;
    case OT_COMMISSIONER_JOINER_CONNECTED:
        otbrLog(OTBR_LOG_INFO, "joiner connected");
        break;
    case OT_COMMISSIONER_JOINER_FINALIZE:
        otbrLog(OTBR_LOG_INFO, "joiner finalize");
        break;
    case OT_COMMISSIONER_JOINER_END:
        otbrLog(OTBR_LOG_INFO, "joiner end");
        break;
    case OT_COMMISSIONER_JOINER_REMOVED:
        otbrLog(OTBR_LOG_INFO, "joiner remove");
        break;
    }
}

int UbusServer::UbusGetInformation(struct ubus_context *     aContext,
                                   struct ubus_object *      aObj,
                                   struct ubus_request_data *aRequest,
                                   const char *              aMethod,
                                   struct blob_attr *        aMsg,
                                   const char *              aAction)
{
    OT_UNUSED_VARIABLE(aObj);
    OT_UNUSED_VARIABLE(aMethod);
    OT_UNUSED_VARIABLE(aMsg);

    otError error = OT_ERROR_NONE;

    blob_buf_init(&mBuf, 0);

    sNcpThreadMutex->lock();
    if (!strcmp(aAction, "networkname"))
        blobmsg_add_string(&mBuf, "NetworkName", otThreadGetNetworkName(mController->GetInstance()));
    else if (!strcmp(aAction, "state"))
    {
        char state[10];
        GetState(mController->GetInstance(), state);
        blobmsg_add_string(&mBuf, "State", state);
    }
    else if (!strcmp(aAction, "channel"))
        blobmsg_add_u32(&mBuf, "Channel", otLinkGetChannel(mController->GetInstance()));
    else if (!strcmp(aAction, "panid"))
    {
        char panIdString[PANID_LENGTH];
        sprintf(panIdString, "0x%04x", otLinkGetPanId(mController->GetInstance()));
        blobmsg_add_string(&mBuf, "PanId", panIdString);
    }
    else if (!strcmp(aAction, "rloc16"))
    {
        char rloc[PANID_LENGTH];
        sprintf(rloc, "0x%04x", otThreadGetRloc16(mController->GetInstance()));
        blobmsg_add_string(&mBuf, "rloc16", rloc);
    }
    else if (!strcmp(aAction, "masterkey"))
    {
        char           outputKey[MASTERKEY_LENGTH] = "";
        const uint8_t *key = reinterpret_cast<const uint8_t *>(otThreadGetMasterKey(mController->GetInstance()));
        OutputBytes(key, OT_MASTER_KEY_SIZE, outputKey);
        blobmsg_add_string(&mBuf, "Masterkey", outputKey);
    }
    else if (!strcmp(aAction, "pskc"))
    {
        char          outputPskc[MASTERKEY_LENGTH] = "";
        const otPskc *pskc                         = otThreadGetPskc(mController->GetInstance());
        OutputBytes(pskc->m8, OT_MASTER_KEY_SIZE, outputPskc);
        blobmsg_add_string(&mBuf, "pskc", outputPskc);
    }
    else if (!strcmp(aAction, "extpanid"))
    {
        char           outputExtPanId[XPANID_LENGTH] = "";
        const uint8_t *extPanId =
            reinterpret_cast<const uint8_t *>(otThreadGetExtendedPanId(mController->GetInstance()));
        OutputBytes(extPanId, OT_EXT_PAN_ID_SIZE, outputExtPanId);
        blobmsg_add_string(&mBuf, "ExtPanId", outputExtPanId);
    }
    else if (!strcmp(aAction, "mode"))
    {
        otLinkModeConfig linkMode;
        char             mode[5] = "";

        memset(&linkMode, 0, sizeof(otLinkModeConfig));

        linkMode = otThreadGetLinkMode(mController->GetInstance());

        if (linkMode.mRxOnWhenIdle)
        {
            strcat(mode, "r");
        }

        if (linkMode.mSecureDataRequests)
        {
            strcat(mode, "s");
        }

        if (linkMode.mDeviceType)
        {
            strcat(mode, "d");
        }

        if (linkMode.mNetworkData)
        {
            strcat(mode, "n");
        }
        blobmsg_add_string(&mBuf, "Mode", mode);
    }
    else if (!strcmp(aAction, "leaderpartitionid"))
    {
        blobmsg_add_u32(&mBuf, "Leaderpartitionid", otThreadGetLocalLeaderPartitionId(mController->GetInstance()));
    }
    else if (!strcmp(aAction, "leaderdata"))
    {
        otLeaderData leaderData;

        SuccessOrExit(error = otThreadGetLeaderData(mController->GetInstance(), &leaderData));

        sJsonUri = blobmsg_open_table(&mBuf, "leaderdata");

        blobmsg_add_u32(&mBuf, "PartitionId", leaderData.mPartitionId);
        blobmsg_add_u32(&mBuf, "Weighting", leaderData.mWeighting);
        blobmsg_add_u32(&mBuf, "DataVersion", leaderData.mDataVersion);
        blobmsg_add_u32(&mBuf, "StableDataVersion", leaderData.mStableDataVersion);
        blobmsg_add_u32(&mBuf, "LeaderRouterId", leaderData.mLeaderRouterId);

        blobmsg_close_table(&mBuf, sJsonUri);
    }
    else if (!strcmp(aAction, "networkdata"))
    {
        ubus_send_reply(aContext, aRequest, mNetworkdataBuf.head);
        if (time(NULL) - mSecond > 10)
        {
            struct otIp6Address address;
            uint8_t             tlvTypes[OT_NETWORK_DIAGNOSTIC_TYPELIST_MAX_ENTRIES];
            uint8_t             count             = 0;
            char                multicastAddr[10] = "ff03::2";
            long                value;

            blob_buf_init(&mNetworkdataBuf, 0);

            SuccessOrExit(error = otIp6AddressFromString(multicastAddr, &address));

            value             = 5;
            tlvTypes[count++] = static_cast<uint8_t>(value);
            value             = 16;
            tlvTypes[count++] = static_cast<uint8_t>(value);

            sBufNum = 0;
            otThreadSendDiagnosticGet(mController->GetInstance(), &address, tlvTypes, count);
            mSecond = time(NULL);
        }
        goto exit;
    }
    else if (!strcmp(aAction, "joinernum"))
    {
        void *       jsonTable = NULL;
        void *       jsonArray = NULL;
        otJoinerInfo joinerInfo;
        uint16_t     iterator        = 0;
        int          joinerNum       = 0;
        char         eui64[EXTPANID] = "";

        blob_buf_init(&mBuf, 0);

        jsonArray = blobmsg_open_array(&mBuf, "joinerList");
        while (otCommissionerGetNextJoinerInfo(mController->GetInstance(), &iterator, &joinerInfo) == OT_ERROR_NONE)
        {
            memset(eui64, 0, sizeof(eui64));

            jsonTable = blobmsg_open_table(&mBuf, NULL);

            blobmsg_add_string(&mBuf, "pskc", joinerInfo.mPsk);
            OutputBytes(joinerInfo.mEui64.m8, sizeof(joinerInfo.mEui64.m8), eui64);
            blobmsg_add_string(&mBuf, "eui64", eui64);
            if (joinerInfo.mAny)
                blobmsg_add_u16(&mBuf, "isAny", 1);
            else
                blobmsg_add_u16(&mBuf, "isAny", 0);

            blobmsg_close_table(&mBuf, jsonTable);

            joinerNum++;
        }
        blobmsg_close_array(&mBuf, jsonArray);

        blobmsg_add_u32(&mBuf, "joinernum", joinerNum);
    }
    else if (!strcmp(aAction, "macfilterstate"))
    {
        otMacFilterAddressMode mode = otLinkFilterGetAddressMode(mController->GetInstance());

        blob_buf_init(&mBuf, 0);

        if (mode == OT_MAC_FILTER_ADDRESS_MODE_DISABLED)
        {
            blobmsg_add_string(&mBuf, "state", "disable");
        }
        else if (mode == OT_MAC_FILTER_ADDRESS_MODE_WHITELIST)
        {
            blobmsg_add_string(&mBuf, "state", "whitelist");
        }
        else if (mode == OT_MAC_FILTER_ADDRESS_MODE_BLACKLIST)
        {
            blobmsg_add_string(&mBuf, "state", "blacklist");
        }
        else
        {
            blobmsg_add_string(&mBuf, "state", "error");
        }
    }
    else if (!strcmp(aAction, "macfilteraddr"))
    {
        otMacFilterEntry    entry;
        otMacFilterIterator iterator = OT_MAC_FILTER_ITERATOR_INIT;

        blob_buf_init(&mBuf, 0);

        sJsonUri = blobmsg_open_array(&mBuf, "addrlist");

        while (otLinkFilterGetNextAddress(mController->GetInstance(), &iterator, &entry) == OT_ERROR_NONE)
        {
            char extAddress[XPANID_LENGTH] = "";
            OutputBytes(entry.mExtAddress.m8, sizeof(entry.mExtAddress.m8), extAddress);
            blobmsg_add_string(&mBuf, "addr", extAddress);
        }

        blobmsg_close_array(&mBuf, sJsonUri);
    }
    else
    {
        perror("invalid argument in get information ubus\n");
    }

    AppendResult(error, aContext, aRequest);
exit:
    sNcpThreadMutex->unlock();
    return 0;
}

void UbusServer::HandleDiagnosticGetResponse(otMessage *aMessage, const otMessageInfo *aMessageInfo, void *aContext)
{
    static_cast<UbusServer *>(aContext)->HandleDiagnosticGetResponse(aMessage,
                                                                     *static_cast<const otMessageInfo *>(aMessageInfo));
}

static bool IsRoutingLocator(const otIp6Address *aAddress)
{
    enum
    {
        kAloc16Mask            = 0xfc, ///< The mask for Aloc16.
        kRloc16ReservedBitMask = 0x02, ///< The mask for the reserved bit of Rloc16.
    };

    return (aAddress->mFields.m32[2] == htonl(0x000000ff) && aAddress->mFields.m16[6] == htons(0xfe00) &&
            aAddress->mFields.m8[14] < kAloc16Mask && (aAddress->mFields.m8[14] & kRloc16ReservedBitMask) == 0);
}

void UbusServer::HandleDiagnosticGetResponse(otMessage *aMessage, const otMessageInfo &aMessageInfo)
{
    uint16_t              rloc16;
    uint16_t              sockRloc16 = 0;
    void *                jsonArray  = NULL;
    void *                jsonItem   = NULL;
    char                  xrloc[10];
    otNetworkDiagTlv      diagTlv;
    otNetworkDiagIterator iterator = OT_NETWORK_DIAGNOSTIC_ITERATOR_INIT;

    char networkdata[20];
    sprintf(networkdata, "networkdata%d", sBufNum);
    sJsonUri = blobmsg_open_table(&mNetworkdataBuf, networkdata);
    sBufNum++;

    if (IsRoutingLocator(&aMessageInfo.mSockAddr))
    {
        sockRloc16 = aMessageInfo.mPeerAddr.mFields.m16[7];
        sprintf(xrloc, "0x%04x", sockRloc16);
        blobmsg_add_string(&mNetworkdataBuf, "rloc", xrloc);
    }

    while (otThreadGetNextDiagnosticTlv(aMessage, &iterator, &diagTlv) == OT_ERROR_NONE)
    {
        switch (diagTlv.mType)
        {
        case OT_NETWORK_DIAGNOSTIC_TLV_ROUTE:
        {
            const otNetworkDiagRoute &route = diagTlv.mData.mRoute;

            jsonArray = blobmsg_open_array(&mNetworkdataBuf, "routedata");

            for (uint16_t i = 0; i < route.mRouteCount; ++i)
            {
                uint8_t in, out;
                in  = route.mRouteData[i].mLinkQualityIn;
                out = route.mRouteData[i].mLinkQualityOut;
                if (in != 0 && out != 0)
                {
                    jsonItem = blobmsg_open_table(&mNetworkdataBuf, "router");
                    rloc16   = route.mRouteData[i].mRouterId << 10;
                    blobmsg_add_u32(&mNetworkdataBuf, "routerid", route.mRouteData[i].mRouterId);
                    sprintf(xrloc, "0x%04x", rloc16);
                    blobmsg_add_string(&mNetworkdataBuf, "rloc", xrloc);
                    blobmsg_close_table(&mNetworkdataBuf, jsonItem);
                }
            }
            blobmsg_close_array(&mNetworkdataBuf, jsonArray);
            break;
        }

        case OT_NETWORK_DIAGNOSTIC_TLV_CHILD_TABLE:
        {
            jsonArray = blobmsg_open_array(&mNetworkdataBuf, "childdata");
            for (uint16_t i = 0; i < diagTlv.mData.mChildTable.mCount; ++i)
            {
                enum
                {
                    kModeRxOnWhenIdle      = 1 << 3, ///< If the device has its receiver on when not transmitting.
                    kModeSecureDataRequest = 1 << 2, ///< If the device uses link layer security for all data requests.
                    kModeFullThreadDevice  = 1 << 1, ///< If the device is an FTD.
                    kModeFullNetworkData   = 1 << 0, ///< If the device requires the full Network Data.
                };
                const otNetworkDiagChildEntry &entry = diagTlv.mData.mChildTable.mTable[i];

                uint8_t mode = 0;

                jsonItem = blobmsg_open_table(&mNetworkdataBuf, "child");
                sprintf(xrloc, "0x%04x", (sockRloc16 | entry.mChildId));
                blobmsg_add_string(&mNetworkdataBuf, "rloc", xrloc);

                mode = (entry.mMode.mRxOnWhenIdle ? kModeRxOnWhenIdle : 0) |
                       (entry.mMode.mSecureDataRequests ? kModeSecureDataRequest : 0) |
                       (entry.mMode.mDeviceType ? kModeFullThreadDevice : 0) |
                       (entry.mMode.mNetworkData ? kModeFullNetworkData : 0);
                blobmsg_add_u16(&mNetworkdataBuf, "mode", mode);
                blobmsg_close_table(&mNetworkdataBuf, jsonItem);
            }
            blobmsg_close_array(&mNetworkdataBuf, jsonArray);
            break;
        }

        default:
            // Ignore other network diagnostics data.
            break;
        }
    }

    blobmsg_close_table(&mNetworkdataBuf, sJsonUri);
}

int UbusServer::UbusSetInformation(struct ubus_context *     aContext,
                                   struct ubus_object *      aObj,
                                   struct ubus_request_data *aRequest,
                                   const char *              aMethod,
                                   struct blob_attr *        aMsg,
                                   const char *              aAction)
{
    OT_UNUSED_VARIABLE(aObj);
    OT_UNUSED_VARIABLE(aMethod);
    OT_UNUSED_VARIABLE(aMsg);

    otError error = OT_ERROR_NONE;

    blob_buf_init(&mBuf, 0);

    sNcpThreadMutex->lock();
    if (!strcmp(aAction, "networkname"))
    {
        struct blob_attr *tb[SET_NETWORK_MAX];

        blobmsg_parse(setNetworknamePolicy, SET_NETWORK_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[SETNETWORK] != NULL)
        {
            char *newName = blobmsg_get_string(tb[SETNETWORK]);
            SuccessOrExit(error = otThreadSetNetworkName(mController->GetInstance(), newName));
        }
    }
    else if (!strcmp(aAction, "channel"))
    {
        struct blob_attr *tb[SET_NETWORK_MAX];

        blobmsg_parse(setChannelPolicy, SET_NETWORK_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[SETNETWORK] != NULL)
        {
            uint32_t channel = blobmsg_get_u32(tb[SETNETWORK]);
            SuccessOrExit(error = otLinkSetChannel(mController->GetInstance(), static_cast<uint8_t>(channel)));
        }
    }
    else if (!strcmp(aAction, "panid"))
    {
        struct blob_attr *tb[SET_NETWORK_MAX];

        blobmsg_parse(setPanIdPolicy, SET_NETWORK_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[SETNETWORK] != NULL)
        {
            long  value;
            char *panid = blobmsg_get_string(tb[SETNETWORK]);
            SuccessOrExit(error = ParseLong(panid, value));
            error = otLinkSetPanId(mController->GetInstance(), static_cast<otPanId>(value));
        }
    }
    else if (!strcmp(aAction, "masterkey"))
    {
        struct blob_attr *tb[SET_NETWORK_MAX];

        blobmsg_parse(setMasterkeyPolicy, SET_NETWORK_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[SETNETWORK] != NULL)
        {
            otMasterKey key;
            char *      masterkey = blobmsg_get_string(tb[SETNETWORK]);

            VerifyOrExit(Hex2Bin(masterkey, key.m8, sizeof(key.m8)) == OT_MASTER_KEY_SIZE, error = OT_ERROR_PARSE);
            SuccessOrExit(error = otThreadSetMasterKey(mController->GetInstance(), &key));
        }
    }
    else if (!strcmp(aAction, "pskc"))
    {
        struct blob_attr *tb[SET_NETWORK_MAX];

        blobmsg_parse(setPskcPolicy, SET_NETWORK_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[SETNETWORK] != NULL)
        {
            otPskc pskc;

            VerifyOrExit(Hex2Bin(blobmsg_get_string(tb[SETNETWORK]), pskc.m8, sizeof(pskc)) == OT_PSKC_MAX_SIZE,
                         error = OT_ERROR_PARSE);
            SuccessOrExit(error = otThreadSetPskc(mController->GetInstance(), &pskc));
        }
    }
    else if (!strcmp(aAction, "extpanid"))
    {
        struct blob_attr *tb[SET_NETWORK_MAX];

        blobmsg_parse(setExtPanIdPolicy, SET_NETWORK_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[SETNETWORK] != NULL)
        {
            otExtendedPanId extPanId;
            char *          input = blobmsg_get_string(tb[SETNETWORK]);
            VerifyOrExit(Hex2Bin(input, extPanId.m8, sizeof(extPanId)) >= 0, error = OT_ERROR_PARSE);
            error = otThreadSetExtendedPanId(mController->GetInstance(), &extPanId);
        }
    }
    else if (!strcmp(aAction, "mode"))
    {
        otLinkModeConfig  linkMode;
        struct blob_attr *tb[SET_NETWORK_MAX];

        blobmsg_parse(setModePolicy, SET_NETWORK_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[SETNETWORK] != NULL)
        {
            char *inputMode = blobmsg_get_string(tb[SETNETWORK]);
            for (char *ch = inputMode; *ch != '\0'; ch++)
            {
                switch (*ch)
                {
                case 'r':
                    linkMode.mRxOnWhenIdle = 1;
                    break;

                case 's':
                    linkMode.mSecureDataRequests = 1;
                    break;

                case 'd':
                    linkMode.mDeviceType = 1;
                    break;

                case 'n':
                    linkMode.mNetworkData = 1;
                    break;

                default:
                    ExitNow(error = OT_ERROR_PARSE);
                }
            }

            SuccessOrExit(error = otThreadSetLinkMode(mController->GetInstance(), linkMode));
        }
    }
    else if (!strcmp(aAction, "leaderpartitionid"))
    {
        struct blob_attr *tb[SET_NETWORK_MAX];

        blobmsg_parse(setLeaderPartitionIdPolicy, SET_NETWORK_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[SETNETWORK] != NULL)
        {
            uint32_t input = blobmsg_get_u32(tb[SETNETWORK]);
            otThreadSetLocalLeaderPartitionId(mController->GetInstance(), input);
        }
    }
    else if (!strcmp(aAction, "macfilteradd"))
    {
        struct blob_attr *tb[SET_NETWORK_MAX];
        otExtAddress      extAddr;

        blobmsg_parse(macfilterAddPolicy, SET_NETWORK_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[SETNETWORK] != NULL)
        {
            char *addr = blobmsg_get_string(tb[SETNETWORK]);

            VerifyOrExit(Hex2Bin(addr, extAddr.m8, OT_EXT_ADDRESS_SIZE) == OT_EXT_ADDRESS_SIZE, error = OT_ERROR_PARSE);

            error = otLinkFilterAddAddress(mController->GetInstance(), &extAddr);

            VerifyOrExit(error == OT_ERROR_NONE || error == OT_ERROR_ALREADY);
        }
    }
    else if (!strcmp(aAction, "macfilterremove"))
    {
        struct blob_attr *tb[SET_NETWORK_MAX];
        otExtAddress      extAddr;

        blobmsg_parse(macfilterRemovePolicy, SET_NETWORK_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[SETNETWORK] != NULL)
        {
            char *addr = blobmsg_get_string(tb[SETNETWORK]);
            VerifyOrExit(Hex2Bin(addr, extAddr.m8, OT_EXT_ADDRESS_SIZE) == OT_EXT_ADDRESS_SIZE, error = OT_ERROR_PARSE);

            SuccessOrExit(error = otLinkFilterRemoveAddress(mController->GetInstance(), &extAddr));
        }
    }
    else if (!strcmp(aAction, "macfiltersetstate"))
    {
        struct blob_attr *tb[SET_NETWORK_MAX];

        blobmsg_parse(macfilterSetStatePolicy, SET_NETWORK_MAX, tb, blob_data(aMsg), blob_len(aMsg));
        if (tb[SETNETWORK] != NULL)
        {
            char *state = blobmsg_get_string(tb[SETNETWORK]);

            if (strcmp(state, "disable") == 0)
            {
                SuccessOrExit(error = otLinkFilterSetAddressMode(mController->GetInstance(),
                                                                 OT_MAC_FILTER_ADDRESS_MODE_DISABLED));
            }
            else if (strcmp(state, "whitelist") == 0)
            {
                SuccessOrExit(error = otLinkFilterSetAddressMode(mController->GetInstance(),
                                                                 OT_MAC_FILTER_ADDRESS_MODE_WHITELIST));
            }
            else if (strcmp(state, "blacklist") == 0)
            {
                SuccessOrExit(error = otLinkFilterSetAddressMode(mController->GetInstance(),
                                                                 OT_MAC_FILTER_ADDRESS_MODE_BLACKLIST));
            }
        }
    }
    else if (!strcmp(aAction, "macfilterclear"))
    {
        otLinkFilterClearAddresses(mController->GetInstance());
    }
    else
    {
        perror("invalid argument in get information ubus\n");
    }

exit:
    sNcpThreadMutex->unlock();
    AppendResult(error, aContext, aRequest);
    return 0;
}

void UbusServer::GetState(otInstance *aInstance, char *aState)
{
    switch (otThreadGetDeviceRole(aInstance))
    {
    case OT_DEVICE_ROLE_DISABLED:
        strcpy(aState, "disabled");
        break;

    case OT_DEVICE_ROLE_DETACHED:
        strcpy(aState, "detached");
        break;

    case OT_DEVICE_ROLE_CHILD:
        strcpy(aState, "child");
        break;

    case OT_DEVICE_ROLE_ROUTER:
        strcpy(aState, "router");
        break;

    case OT_DEVICE_ROLE_LEADER:
        strcpy(aState, "leader");
        break;
    default:
        strcpy(aState, "invalid aState");
        break;
    }
}

void UbusServer::UbusAddFd()
{
    // ubus library function
    ubus_add_uloop(mContext);

#ifdef FD_CLOEXEC
    fcntl(mContext->sock.fd, F_SETFD, fcntl(mContext->sock.fd, F_GETFD) | FD_CLOEXEC);
#endif
}

void UbusServer::UbusReconnTimer(struct uloop_timeout *aTimeout)
{
    GetInstance().UbusReconnTimerDetail(aTimeout);
}

void UbusServer::UbusReconnTimerDetail(struct uloop_timeout *aTimeout)
{
    OT_UNUSED_VARIABLE(aTimeout);

    static struct uloop_timeout retry = {
        list : {},
        pending : false,
        cb : UbusReconnTimer,
        time : {},
    };
    int time = 2;

    if (ubus_reconnect(mContext, mSockPath) != 0)
    {
        uloop_timeout_set(&retry, time * 1000);
        return;
    }

    UbusAddFd();
}

void UbusServer::UbusConnectionLost(struct ubus_context *aContext)
{
    OT_UNUSED_VARIABLE(aContext);

    UbusReconnTimer(NULL);
}

int UbusServer::DisplayUbusInit(const char *aPath)
{
    uloop_init();
    signal(SIGPIPE, SIG_IGN);

    mSockPath = aPath;

    mContext = ubus_connect(aPath);
    if (!mContext)
    {
        otbrLog(OTBR_LOG_ERR, "ubus connect failed");
        return -1;
    }

    otbrLog(OTBR_LOG_INFO, "connected as %08x\n", mContext->local_id);
    mContext->connection_lost = UbusConnectionLost;

    /* file description */
    UbusAddFd();

    /* Add a object */
    if (ubus_add_object(mContext, &otbr) != 0)
    {
        otbrLog(OTBR_LOG_ERR, "ubus add obj failed");
        return -1;
    }

    return 0;
}

void UbusServer::DisplayUbusDone(void)
{
    if (mContext)
    {
        ubus_free(mContext);
        mContext = NULL;
    }
}

void UbusServer::InstallUbusObject(void)
{
    char *path = NULL;

    if (-1 == DisplayUbusInit(path))
    {
        otbrLog(OTBR_LOG_ERR, "ubus connect failed");
        return;
    }

    otbrLog(OTBR_LOG_INFO, "uloop run");
    uloop_run();

    DisplayUbusDone();

    uloop_done();
}

otError UbusServer::ParseLong(char *aString, long &aLong)
{
    char *endptr;
    aLong = strtol(aString, &endptr, 0);
    return (*endptr == '\0') ? OT_ERROR_NONE : OT_ERROR_PARSE;
}

int UbusServer::Hex2Bin(const char *aHex, uint8_t *aBin, uint16_t aBinLength)
{
    size_t      hexLength = strlen(aHex);
    const char *hexEnd    = aHex + hexLength;
    uint8_t *   cur       = aBin;
    uint8_t     numChars  = hexLength & 1;
    uint8_t     byte      = 0;
    int         rval;

    VerifyOrExit((hexLength + 1) / 2 <= aBinLength, rval = -1);

    while (aHex < hexEnd)
    {
        if ('A' <= *aHex && *aHex <= 'F')
        {
            byte |= 10 + (*aHex - 'A');
        }
        else if ('a' <= *aHex && *aHex <= 'f')
        {
            byte |= 10 + (*aHex - 'a');
        }
        else if ('0' <= *aHex && *aHex <= '9')
        {
            byte |= *aHex - '0';
        }
        else
        {
            ExitNow(rval = -1);
        }

        aHex++;
        numChars++;

        if (numChars >= 2)
        {
            numChars = 0;
            *cur++   = byte;
            byte     = 0;
        }
        else
        {
            byte <<= 4;
        }
    }

    rval = static_cast<int>(cur - aBin);

exit:
    return rval;
}

} // namespace ubus
} // namespace otbr

void UbusServerInit(otbr::Ncp::ControllerOpenThread *aController, std::mutex *aNcpThreadMutex)
{
    otbr::ubus::sNcpThreadMutex = aNcpThreadMutex;
    otbr::ubus::sUbusEfd        = eventfd(0, 0);

    otbr::ubus::UbusServer::Initialize(aController);

    if (otbr::ubus::sUbusEfd == -1)
    {
        perror("Failed to create eventfd for ubus");
        exit(EXIT_FAILURE);
    }
}

void UbusServerRun(void)
{
    otbr::ubus::UbusServer::GetInstance().InstallUbusObject();
}

void UbusUpdateFdSet(fd_set &aReadFdSet, int &aMaxFd)
{
    VerifyOrExit(otbr::ubus::sUbusEfd != -1);

    FD_SET(otbr::ubus::sUbusEfd, &aReadFdSet);

    if (aMaxFd < otbr::ubus::sUbusEfd)
    {
        aMaxFd = otbr::ubus::sUbusEfd;
    }

exit:
    return;
}

void UbusProcess(const fd_set &aReadFdSet)
{
    ssize_t  retval;
    uint64_t num;

    VerifyOrExit(otbr::ubus::sUbusEfd != -1);

    if (FD_ISSET(otbr::ubus::sUbusEfd, &aReadFdSet))
    {
        retval = read(otbr::ubus::sUbusEfd, &num, sizeof(uint64_t));
        if (retval != sizeof(uint64_t))
        {
            perror("read ubus eventfd failed\n");
            exit(EXIT_FAILURE);
        }
        goto exit;
    }

exit:
    return;
}
