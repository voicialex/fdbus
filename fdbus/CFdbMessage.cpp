/*
 * Copyright (C) 2015   Jeremy Chen jeremy_cz@yahoo.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <common_base/CFdbMessage.h>
#include <common_base/CBaseEndpoint.h>
#include <common_base/CFdbContext.h>
#include <common_base/CNanoTimer.h>
#include <common_base/CFdbSession.h>
#include <common_base/CLogProducer.h>
#include <common_base/CFdbBaseObject.h>
#include <idl-gen/common.base.MessageHeader.pb.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/coded_stream.h>
#include <utils/Log.h>

#define FDB_MSG_TX_SYNC         (1 << 0)
#define FDB_MSG_TX_NO_REPLY     (1 << 1)

class CMessageTimer : public CBaseLoopTimer
{
public:
    CMessageTimer(int32_t interval)
        : CBaseLoopTimer(interval, false)
        , mSession(0)
        , mMsgSn(FDB_INVALID_ID)
    {}
protected:
    void run()
    {
        mSession->terminateMessage(mMsgSn, NFdbBase::FDB_ST_TIMEOUT, "Message is destroyed due to timeout.");
    }
private:
    CFdbSession *mSession;
    FdbMsgSn_t mMsgSn;
    friend class CFdbMessage;
};

CFdbMessage::CFdbMessage(FdbMsgCode_t code, EFdbMessageEncoding enc)
    : mType(NFdbBase::MT_REQUEST)
    , mCode(code)
    , mSn(FDB_INVALID_ID)
    , mPayloadSize(0)
    , mHeadSize(0)
    , mOffset(0)
    , mExtraSize(0)
    , mSid(FDB_INVALID_ID)
    , mOid(FDB_INVALID_ID)
    , mBuffer(0)
    , mFlag((enc << MSG_FLAG_ENCODING) & MSG_FLAG_ENCODING_MASK)
    , mTimer(0)
    , mStringData(0)
{
}

CFdbMessage::CFdbMessage(FdbMsgCode_t code, CFdbBaseObject *obj, FdbSessionId_t alt_receiver, EFdbMessageEncoding enc)
    : mType(NFdbBase::MT_REQUEST)
    , mCode(code)
    , mSn(FDB_INVALID_ID)
    , mPayloadSize(0)
    , mHeadSize(0)
    , mOffset(0)
    , mExtraSize(0)
    , mBuffer(0)
    , mFlag((enc << MSG_FLAG_ENCODING) & MSG_FLAG_ENCODING_MASK)
    , mTimer(0)
    , mStringData(0)
{
    setDestination(obj, alt_receiver);
}

CFdbMessage::CFdbMessage(FdbMsgCode_t code, CFdbMessage *msg, EFdbMessageEncoding enc)
    : mType(NFdbBase::MT_BROADCAST)
    , mCode(code)
    , mSn(msg->mSn)
    , mPayloadSize(0)
    , mHeadSize(0)
    , mOffset(0)
    , mExtraSize(0)
    , mSid(msg->mSid)
    , mOid(msg->mOid)
    , mBuffer(0)
    , mFlag((enc << MSG_FLAG_ENCODING) & MSG_FLAG_ENCODING_MASK)
    , mTimer(0)
    , mSenderName(msg->mSenderName)
    , mStringData(0)
{
}

CFdbMessage::CFdbMessage(NFdbBase::FdbMessageHeader &head
                         , CFdbMsgPrefix &prefix
                         , uint8_t *buffer
                         , FdbSessionId_t sid
                        )
    : mType(NFdbBase::MT_REPLY)
    , mCode(head.code())
    , mSn(head.serial_number())
    , mPayloadSize(head.payload_size())
    , mHeadSize(prefix.mHeadLength)
    , mOffset(0)
    , mExtraSize(prefix.mTotalLength - mPrefixSize - mHeadSize - mPayloadSize)
    , mSid(sid)
    , mOid(head.object_id())
    , mBuffer(buffer)
    , mFlag((head.flag() & MSG_GLOBAL_FLAG_MASK) | MSG_FLAG_EXTERNAL_BUFFER)
    , mTimer(0)
    , mStringData(0)
{
    if (mExtraSize < 0)
    {
        mExtraSize = 0;
        LOG_E("CFdbMessage: mExtraSize is less than 0: %d %d %d\n",
                prefix.mTotalLength, mHeadSize, mPayloadSize);
    }
    if (head.has_sender_name())
    {
        mSenderName = head.sender_name().c_str();
    }
};

CFdbMessage::~CFdbMessage()
{
    if (mTimer)
    {
        delete mTimer;
        mTimer = 0;
    }
    releaseBuffer();
    if (mStringData)
    {
        delete mStringData;
        mStringData = 0;
    }
    //LOG_I("Message %d is destroyed!\n", (int32_t)mSn);
}

void CFdbMessage::setDestination(CFdbBaseObject *obj, FdbSessionId_t alt_sid)
{
    FdbSessionId_t sid = obj->getDefaultSession();
    if (isValidFdbId(alt_sid))
    {
        mSid = alt_sid;
        mFlag &= ~MSG_FLAG_ENDPOINT;
    }
    else if (isValidFdbId(sid))
    {
        mSid = sid;
        mFlag &= ~MSG_FLAG_ENDPOINT;
    }
    else
    {
        mEpid = obj->epid();
        mFlag |= MSG_FLAG_ENDPOINT;
    }
    mOid = obj->objId();
    //mSenderName = obj->name();
}

void CFdbMessage::run(CBaseWorker *worker, Ptr &ref)
{
    switch (mType)
    {
        case NFdbBase::MT_REQUEST:
        case NFdbBase::MT_SIDEBAND_REQUEST:
            doRequest(ref);
            break;
        case NFdbBase::MT_REPLY:
        case NFdbBase::MT_SIDEBAND_REPLY:
            doReply(ref);
            break;
        case NFdbBase::MT_BROADCAST:
            doBroadcast(ref);
            break;
        case NFdbBase::MT_STATUS:
            doStatus(ref);
            break;
        case NFdbBase::MT_SUBSCRIBE_REQ:
            if ((mCode == FDB_CODE_SUBSCRIBE) || (mCode == FDB_CODE_UPDATE))
			{
                doSubscribeReq(ref);
            }
            else if (mCode == FDB_CODE_UNSUBSCRIBE)
            {
                doUnsubscribeReq(ref);
            }
            break;
        default:
            LOG_E("CFdbMessage: Message %d: Unknown type!\n", (int32_t)mSn);
            break;
    }
}

bool CFdbMessage::feedback(CBaseJob::Ptr &msg_ref
                         , const CFdbBasePayload &data
                         , NFdbBase::wrapper::FdbMessageType type)
{
    if (mFlag & MSG_FLAG_NOREPLY_EXPECTED)
    {
        return false;
    }
    if (!serialize(data))
    {
        return false;
    }
    mType = type;
    mFlag &= ~MSG_FLAG_ENCODING_MASK;
    mFlag |= MSG_FLAG_REPLIED | MSG_FLAG_ENC_PROTOBUF;
    if (!CFdbContext::getInstance()->sendAsyncEndeavor(msg_ref))
    {
        mFlag &= ~MSG_FLAG_REPLIED;
        LOG_E("CFdbMessage: Fail to send message job to FDB_CONTEXT!\n");
        return false;
    }
    return true;
}


bool CFdbMessage::reply(CBaseJob::Ptr &msg_ref, const CFdbBasePayload &data)
{
    CFdbMessage *fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
    return fdb_msg->feedback(msg_ref, data, NFdbBase::MT_REPLY);
}

bool CFdbMessage::reply(CBaseJob::Ptr &msg_ref
                      , const void *buffer
                      , int32_t size
                      , EFdbMessageEncoding enc
                      , const char *log_data)
{
    CFdbMessage *fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
    if (fdb_msg->mFlag & MSG_FLAG_NOREPLY_EXPECTED)
    {
        return false;
    }
    if (!fdb_msg->serialize((uint8_t *)buffer, size))
    {
        return false;
    }
    fdb_msg->setLogData(log_data);
    
    fdb_msg->mType = NFdbBase::MT_REPLY;
    fdb_msg->mFlag &= ~MSG_FLAG_ENCODING_MASK;
    fdb_msg->mFlag |= MSG_FLAG_REPLIED | ((enc << MSG_FLAG_ENCODING) & MSG_FLAG_ENCODING_MASK);
    if (!CFdbContext::getInstance()->sendAsyncEndeavor(msg_ref))
    {
        fdb_msg->mFlag &= ~MSG_FLAG_REPLIED;
        LOG_E("CFdbMessage: Fail to send reply job to FDB_CONTEXT!\n");
        return false;
    }
    return true;
}

bool CFdbMessage::status(CBaseJob::Ptr &msg_ref, int32_t error_code, const char *description)
{
    CFdbMessage *fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
    if (fdb_msg->mFlag & MSG_FLAG_NOREPLY_EXPECTED)
    {
        return false;
    }
    fdb_msg->setErrorMsg(NFdbBase::MT_STATUS, error_code, description);
    if (!CFdbContext::getInstance()->sendAsyncEndeavor(msg_ref))
    {
        LOG_E("CFdbMessage: Fail to send status job to FDB_CONTEXT!\n");
        return false;
    }
    return true;
}

bool CFdbMessage::submit(CBaseJob::Ptr &msg_ref
                         , uint32_t tx_flag
                         , int32_t timeout)
{
    bool sync = !!(tx_flag & FDB_MSG_TX_SYNC);
    if (sync && FDB_CONTEXT->isSelf())
    {
        LOG_E("CFdbMessage: Cannot send sychronously from FDB_CONTEXT!\n");
        return false;
    }

    if (tx_flag & FDB_MSG_TX_NO_REPLY)
    {
        mFlag |= MSG_FLAG_NOREPLY_EXPECTED;
    }
    else
    {
        mFlag |= MSG_FLAG_AUTO_REPLY;
        if (sync)
        {
            mFlag |= MSG_FLAG_SYNC_REPLY;
        }
        if (timeout > 0)
        {
            mTimer = new CMessageTimer(timeout);
        }
    }

    bool ret;
    if (sync)
    {
        ret = CFdbContext::getInstance()->sendSync(msg_ref);
    }
    else
    {
        ret = CFdbContext::getInstance()->sendAsync(msg_ref);
    }
    if (!ret)
    {
        LOG_E("CFdbMessage: Fail to send job to FDB_CONTEXT!\n");
    }
    return ret;
}

bool CFdbMessage::invoke(CBaseJob::Ptr &msg_ref
                         , uint32_t tx_flag
                         , int32_t timeout)
{
    mType = NFdbBase::MT_REQUEST;
    return submit(msg_ref, tx_flag, timeout);
}

bool CFdbMessage::invoke(int32_t timeout)
{
    CBaseJob::Ptr msg_ref(this);
    return invoke(msg_ref, 0, timeout);
}

bool CFdbMessage::invoke(CBaseJob::Ptr &msg_ref
                         , int32_t timeout)
{
    CFdbMessage *msg = castToMessage<CFdbMessage *>(msg_ref);
    return msg ? msg->invoke(msg_ref, FDB_MSG_TX_SYNC, timeout) : false;
}

bool CFdbMessage::send()
{
    CBaseJob::Ptr msg_ref(this);
    return invoke(msg_ref, FDB_MSG_TX_NO_REPLY, -1);
}

bool CFdbMessage::sendLog(const CFdbBasePayload &data
                           , uint8_t *log_data
                           , int32_t size
                           , int32_t clipped_size
                           , bool send_as_job)
{
    mFlag |= MSG_FLAG_NOREPLY_EXPECTED;
    mFlag &= ~MSG_FLAG_ENABLE_LOG;
    mType = NFdbBase::MT_REQUEST;

    if (clipped_size < 0)
    {
        mExtraSize = size;
    }
    else
    {
        CFdbMsgPrefix prefix;
        prefix.deserialize(log_data);
        int32_t payload_size = prefix.mTotalLength - prefix.mHeadLength - mPrefixSize;
        if (clipped_size == 0)
        {
            prefix.mTotalLength = mPrefixSize + prefix.mHeadLength;
            mExtraSize = prefix.mTotalLength;
            prefix.serialize(log_data);
        }
        else if (clipped_size < payload_size)
        {
            prefix.mTotalLength = mPrefixSize + prefix.mHeadLength + clipped_size;
            mExtraSize = prefix.mTotalLength;
            prefix.serialize(log_data);
        }
        else
        {
            mExtraSize = size;
        }
    }

    if (!serialize(data))
    {
        return false;
    }

    if (log_data && size)
    {
        memcpy(getExtraBuffer(), log_data, mExtraSize);
    }

    if (send_as_job)
    {
        return CFdbContext::getInstance()->sendAsync(this);
    }
    else
    {
        CFdbSession *session = getSession();
        if (session)
        {
            return session->sendMessage(this);
        }
    }
    return false;
}

void CFdbMessage::broadcastLog(const CFdbBasePayload &data
                                , const uint8_t *log_data
                                , int32_t size
                                , bool send_as_job)
{
    mType = NFdbBase::MT_BROADCAST;
    mFlag &= ~MSG_FLAG_ENABLE_LOG;
    mExtraSize = size;
    if (!serialize(data))
    {
        return;
    }

    if (log_data && size)
    {
        memcpy(getExtraBuffer(), log_data, mExtraSize);
    }

    if (send_as_job)
    {
        CFdbContext::getInstance()->sendAsyncEndeavor(this);
    }
    else
    {
        CBaseEndpoint *endpoint = CFdbContext::getInstance()->getEndpoint(mEpid);
        if (endpoint)
        {
            // Broadcast per object!!!
            CFdbBaseObject *object = endpoint->getObject(this, true);
            if (object)
            {
                object->broadcast(this);
            }
        }
    }
}

CFdbMessage *CFdbMessage::parseFdbLog(uint8_t *buffer, int32_t size)
{
    if (!buffer || !size)
    {
        return 0;
    }

    CFdbMessage::CFdbMsgPrefix prefix;
    prefix.deserialize(buffer);
    
    NFdbBase::FdbMessageHeader head;
    if (!CFdbMessage::deserializePb(head,
                                    buffer + CFdbMessage::mPrefixSize,
                                    prefix.mHeadLength))
    {
        LOG_E("CFdbMessage: Unable to deserialize log header!\n");
        return 0;
    }
    int32_t payload_size = head.payload_size();
    int32_t extra_size = prefix.mTotalLength - mPrefixSize - prefix.mHeadLength - payload_size;
    if (extra_size < 0)
    {
        payload_size += extra_size;
        if (payload_size < 0)
        {
            return 0;
        }
        head.set_payload_size(payload_size);
    }
    return new CFdbMessage(head, prefix, buffer, session());
}

bool CFdbMessage::broadcast(FdbMsgCode_t code
                           , const CFdbBasePayload &data
                           , const char *filter)
{
    CBaseMessage *msg = new CFdbBroadcastMsg(code, this, filter);
    msg->mFlag |= mFlag & MSG_FLAG_ENABLE_LOG;
    if (!msg->serialize(data))
    {
        delete msg;
        return false;
    }
    return msg->broadcast();
}

bool CFdbMessage::broadcast(FdbMsgCode_t code
                           , const char *filter
                           , const void *buffer
                           , int32_t size
                           , EFdbMessageEncoding enc
                           , const char *log_data)
{
    CBaseMessage *msg = new CFdbBroadcastMsg(code, this, filter, enc);
    msg->mFlag |= mFlag & MSG_FLAG_ENABLE_LOG;
    if (!msg->serialize(buffer, size))
    {
        delete msg;
        return false;
    }
    msg->setLogData(log_data);
    return msg->broadcast();
}

bool CFdbMessage::broadcast()
{
   mType = NFdbBase::MT_BROADCAST;
   if (!CFdbContext::getInstance()->sendAsyncEndeavor(this))
   {
       LOG_E("CFdbMessage: Fail to send broadcast job to FDB_CONTEXT!\n");
       return false;
   }
   return true;
}

bool CFdbMessage::subscribe(CBaseJob::Ptr &msg_ref
                            , uint32_t tx_flag
                            , FdbMsgCode_t subscribe_code
                            , int32_t timeout)
{
    mType = NFdbBase::MT_SUBSCRIBE_REQ;
    mCode = subscribe_code;
    return submit(msg_ref, tx_flag, timeout);
}

bool CFdbMessage::subscribe(int32_t timeout)
{
    CBaseJob::Ptr msg_ref(this);
    return subscribe(msg_ref, 0, FDB_CODE_SUBSCRIBE, timeout);
}

bool CFdbMessage::subscribe(CBaseJob::Ptr &msg_ref, int32_t timeout)
{
    CFdbMessage *msg = castToMessage<CFdbMessage *>(msg_ref);
    return msg ? msg->subscribe(msg_ref, FDB_MSG_TX_SYNC, FDB_CODE_SUBSCRIBE, timeout) : false;
}

bool CFdbMessage::unsubscribe()
{
    CBaseJob::Ptr msg_ref(this);
    return subscribe(msg_ref, FDB_MSG_TX_NO_REPLY, FDB_CODE_UNSUBSCRIBE, 0);
}

bool CFdbMessage::update(int32_t timeout)
{
    CBaseJob::Ptr msg_ref(this);
    // actually subscribe nothing but just trigger a broadcast() from onBroadcast()
    return subscribe(msg_ref, 0, FDB_CODE_UPDATE, timeout);
}

bool CFdbMessage::update(CBaseJob::Ptr &msg_ref
                            , int32_t timeout)
{
    CFdbMessage *msg = castToMessage<CFdbMessage *>(msg_ref);
    // actually subscribe nothing but just trigger a broadcast() from onBroadcast()
    return msg ? msg->subscribe(msg_ref, FDB_MSG_TX_SYNC, FDB_CODE_UPDATE, timeout) : false;
}

bool CFdbMessage::buildHeader(CFdbSession *session)
{
    if (mFlag & MSG_FLAG_HEAD_OK)
    {
        return true;
    }
    NFdbBase::FdbMessageHeader msg_hdr;
    msg_hdr.set_type((NFdbBase::FdbMessageType)mType);
    msg_hdr.set_serial_number(mSn);
    msg_hdr.set_code(mCode);
    msg_hdr.set_flag(mFlag & MSG_GLOBAL_FLAG_MASK);
    msg_hdr.set_object_id(mOid);
    msg_hdr.set_payload_size(mPayloadSize);

    encodeDebugInfo(msg_hdr, session);
    if (mSenderName.empty())
    {
        msg_hdr.set_sender_name(session->getEndpointName());
    }
    else
    {
        msg_hdr.set_sender_name(mSenderName);
    }

    if (mType == NFdbBase::MT_BROADCAST)
    {
        const char *filter = getFilter();
        if (filter && (filter[0] != '\0'))
        {
            msg_hdr.set_broadcast_filter(filter);
        }
    }

    int32_t head_size = msg_hdr.ByteSize();
    if (head_size > mMaxHeadSize)
    {
        LOG_E("CFdbMessage: Message %d of Session %d: Head is too long!\n", (int32_t)mCode, (int32_t)mSid);
        return false;
    }
    mHeadSize = head_size;
    int32_t head_offset = maxReservedSize() - head_size;
    int32_t prefix_offset = head_offset - mPrefixSize;
    mOffset = prefix_offset;

    try
    {
        google::protobuf::io::ArrayOutputStream aos(mBuffer + head_offset, head_size);
        google::protobuf::io::CodedOutputStream coded_output(&aos);
        if (!msg_hdr.SerializeToCodedStream(&coded_output))
        {
            LOG_E("CFdbMessage: Unable to serialize head!\n");
            return false;
        }
    }
    catch (...)
    {
        LOG_E("CFdbMessage: Unable to serialize head!\n");
        return false;
    }

    // Update offset and head size according to actual head size
    CFdbMsgPrefix prefix(getRawDataSize(), mHeadSize);
    prefix.serialize(getRawBuffer());

    mFlag |= MSG_FLAG_HEAD_OK;
    return true;
}

void CFdbMessage::freeRawBuffer()
{
    if (mBuffer)
    {
        delete[] mBuffer;
        mBuffer = 0;
    }
}

bool CFdbMessage::allocCopyRawBuffer(const void *src, int32_t payload_size)
{
    int32_t total_size = maxReservedSize() + payload_size + mExtraSize;
    mBuffer = new uint8_t[total_size];
    if (src)
    {
        memcpy(mBuffer +  maxReservedSize(), src, payload_size);
    }
    return true;
}

bool CFdbMessage::serialize(const CFdbBasePayload &data, const CFdbBaseObject *object)
{
    mOffset = 0;
    mHeadSize = mMaxHeadSize;

    releaseBuffer();

    try
    {
        mFlag &= ~MSG_FLAG_EXTERNAL_BUFFER;
        mPayloadSize = data.ByteSize();
        mBuffer = new uint8_t[maxReservedSize() + mPayloadSize + mExtraSize];
        google::protobuf::io::ArrayOutputStream aos(
            mBuffer + maxReservedSize(), mPayloadSize);
        google::protobuf::io::CodedOutputStream coded_output(&aos);
        if (!data.SerializeToCodedStream(&coded_output))
        {
            LOG_E("CFdbMessage: Unable to serialize message!\n");
            return false;
        }
    }
    catch (...)
    {
        LOG_E("CFdbMessage: Unable to serialize message!\n");
        return false;
    }

    if (object)
    {
        checkLogEnabled(object);
    }

    if (mFlag & MSG_FLAG_ENABLE_LOG)
    {
        CLogProducer *logger = CFdbContext::getInstance()->getLogger();
        if (logger)
        {
            std::string *log_data = new std::string();
            if (logger->printToString(log_data, data))
            {
                setLogData(log_data);
            }
            else
            {
                delete log_data;
            }
        }
    }

	return true;
}

bool CFdbMessage::serialize(const void *buffer, int32_t size, const CFdbBaseObject *object)
{
    mOffset = 0;
    mHeadSize = mMaxHeadSize;

    if (object)
    {
        checkLogEnabled(object);
    }
    releaseBuffer();
    
    mFlag |= MSG_FLAG_EXTERNAL_BUFFER;
    mPayloadSize = size;
    return allocCopyRawBuffer(buffer, mPayloadSize);
}

void CFdbMessage::releaseBuffer()
{
    if (mFlag & MSG_FLAG_EXTERNAL_BUFFER)
    {
        freeRawBuffer();
    }
    else if (mBuffer)
    {
        delete[] mBuffer;
        mBuffer = 0;
    }
}

void CFdbMessage::replaceBuffer(uint8_t *buffer, int32_t payload_size,
                                int32_t head_size, int32_t offset)
{
    releaseBuffer();
    mBuffer = buffer;
    mPayloadSize = payload_size;
    mHeadSize = head_size;
    mOffset = offset;
}

void CFdbMessage::doRequest(Ptr &ref)
{
    bool success = true;
    const char *reason;
    CFdbSession *session = getSession();
    if (session)
    {
        if (mFlag & MSG_FLAG_NOREPLY_EXPECTED)
        {
            success = session->sendMessage(this);
            reason = "error when sending message!";
        }
        else
        {
            if (session->sendMessage(ref) && mTimer)
            {
                mTimer->mSession = session;
                //TODO: store ref rather than sn for performance
                mTimer->mMsgSn = mSn;
                mTimer->attach(CFdbContext::getInstance());
            }
        }
    }
    else
    {
        success = false;
        reason = "Invalid sid!";
    }
    
    if (!success)
    {
        if (mFlag & MSG_FLAG_SYNC_REPLY)
        {
            setErrorMsg(NFdbBase::MT_UNKNOWN, NFdbBase::FDB_ST_INVALID_ID, reason);
        }
        else
        {
            onAsyncError(ref, NFdbBase::FDB_ST_INVALID_ID, reason);
        }
    }
}


void CFdbMessage::doReply(Ptr &ref)
{
    if (!(mFlag & MSG_FLAG_NOREPLY_EXPECTED))
    {
        CFdbSession *session = getSession();
        if (session)
        {
            session->sendMessage(this);
        }
    }
}

void CFdbMessage::doBroadcast(Ptr &ref)
{
    bool success = true;
    const char *reason = "";
    if (mFlag & MSG_FLAG_ENDPOINT)
    {
        CBaseEndpoint *endpoint = CFdbContext::getInstance()->getEndpoint(mEpid);
        if (endpoint)
        {
            CFdbBaseObject *object = endpoint->getObject(this, true);
            if (object)
            {
                // broadcast to all sessions of the object
                object->broadcast(this);
            }
            else
            {
                success = false;
                reason = "Invalid object id!";
            }
        }
        else
        {
            success = false;
            reason = "Invalid epid!";
        }
    }
    else
    {
        mFlag |= MSG_FLAG_INITIAL_RESPONSE; // mark as initial response
        CFdbSession *session = CFdbContext::getInstance()->getSession(mSid);
        if (session)
        {
            CFdbBaseObject *object =
                    session->container()->owner()->getObject(this, true);
            if (object)
            {
                // Broadcast to specified session of object
                if (!object->broadcast(this, session))
                {
                    success = false;
                    reason = "Not subscribed or fail to send!";
                }
            }
            else
            {
                success = false;
                reason = "Invalid object id!";
            }
        }
        else
        {
            success = false;
            reason = "Invalid sid!";
        }
    }
    if (!success)
    {
        onAsyncError(ref, NFdbBase::FDB_ST_INVALID_ID, reason);
    }
}

void CFdbMessage::doStatus(Ptr &ref)
{
    doReply(ref);
}

void CFdbMessage::doSubscribeReq(Ptr &ref)
{
    doRequest(ref);
}

void CFdbMessage::doUnsubscribeReq(Ptr &ref)
{
    doRequest(ref);
}

void CFdbMessage::setErrorMsg(NFdbBase::wrapper::FdbMessageType type, int32_t error_code, const char *description)
{
    if (type != NFdbBase::MT_UNKNOWN)
    {
        mType = type;
    }

    if ((error_code < NFdbBase::FDB_ST_AUTO_REPLY_OK) || (error_code > NFdbBase::FDB_ST_OK))
    {
        mFlag |= MSG_FLAG_ERROR;
    }

    mFlag |= (MSG_FLAG_STATUS | MSG_FLAG_REPLIED);

    NFdbBase::FdbMsgErrorInfo error_info;
    error_info.set_error_code(error_code);
    if (description)
    {
        error_info.set_description(description);
    }
    serialize(error_info);
}

void CFdbMessage::sendStatus(CFdbSession *session, int32_t error_code, const char *description)
{
    if (!(mFlag & MSG_FLAG_NOREPLY_EXPECTED))
    {
        setErrorMsg(NFdbBase::MT_STATUS, error_code, description);
        session->sendMessage(this);
    }
}

void CFdbMessage::sendAutoReply(CFdbSession *session, int32_t error_code, const char *description)
{
    if ((mFlag & (MSG_FLAG_AUTO_REPLY | MSG_FLAG_REPLIED)) == MSG_FLAG_AUTO_REPLY)
    {
        sendStatus(session, error_code, description);
    }
}

void CFdbMessage::autoReply(CFdbSession *session
                            , CBaseJob::Ptr &msg_ref
                            , int32_t error_code
                            , const char *description)
{
    CFdbMessage *fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
    if (((fdb_msg->mFlag & (MSG_FLAG_AUTO_REPLY | MSG_FLAG_REPLIED)) == MSG_FLAG_AUTO_REPLY)
            && !(fdb_msg->mFlag & MSG_FLAG_NOREPLY_EXPECTED)
            && (msg_ref.use_count() == 1))
    {
        fdb_msg->sendStatus(session, error_code, description);
    }
}

void CFdbMessage::autoReply(CBaseJob::Ptr &msg_ref, int32_t error_code, const char *description)
{
    CFdbMessage *fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
    if (((fdb_msg->mFlag & (MSG_FLAG_AUTO_REPLY | MSG_FLAG_REPLIED)) == MSG_FLAG_AUTO_REPLY)
            && !(fdb_msg->mFlag & MSG_FLAG_NOREPLY_EXPECTED)
            && (msg_ref.use_count() == 1))
    {
        CFdbMessage *fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
        fdb_msg->setErrorMsg(NFdbBase::MT_STATUS, error_code, description);
        CFdbContext::getInstance()->sendAsyncEndeavor(msg_ref);
    }
}

void CFdbMessage::parseTimestamp(const CFdbMsgMetadata &metadata
                               , uint64_t &client_to_server
                               , uint64_t &server_to_reply
                               , uint64_t &reply_to_client
                               , uint64_t &total)
{
    CNanoTimer timer;
    if (!metadata.mSendTime || !metadata.mArriveTime)
    {
        client_to_server = 0;
    }
    else
    {
        timer.startTimer(metadata.mSendTime);
        timer.stopTimer(metadata.mArriveTime);
        client_to_server = timer.getTotalMicroseconds();
    }

    if (!metadata.mArriveTime || !metadata.mReplyTime)
    {
        server_to_reply = 0;
    }
    else
    {
        timer.reset();
        timer.startTimer(metadata.mArriveTime);
        timer.stopTimer(metadata.mReplyTime);
        server_to_reply = timer.getTotalMicroseconds();
    }

    if (!metadata.mReplyTime || !metadata.mReceiveTime)
    {
        reply_to_client = 0;
    }
    else
    {
        timer.reset();
        timer.startTimer(metadata.mReplyTime);
        timer.stopTimer(metadata.mReceiveTime);
        reply_to_client = timer.getTotalMicroseconds();
    }

    if (!metadata.mSendTime || !metadata.mReceiveTime)
    {
        total = 0;
    }
    else
    {
        timer.reset();
        timer.startTimer(metadata.mSendTime);
        timer.stopTimer(metadata.mReceiveTime);
        total = timer.getTotalMicroseconds();
    }
}

bool CFdbMessage::decodeStatus(int32_t &error_code, std::string &description)
{
    NFdbBase::FdbMsgErrorInfo error_msg;
    bool ret = deserialize(error_msg);
    if (ret)
    {
        error_code = error_msg.error_code();
        description = error_msg.description();
    }
    return ret;
}

void CFdbMessage::update(NFdbBase::FdbMessageHeader &head, CFdbMessage::CFdbMsgPrefix &prefix)
{
    mFlag = (mFlag & ~MSG_GLOBAL_FLAG_MASK) | (head.flag() & MSG_GLOBAL_FLAG_MASK);
    //mCode = head.code();
}

CFdbSession *CFdbMessage::getSession()
{
    CFdbSession *session;
    if (mFlag & MSG_FLAG_ENDPOINT)
    {
        CBaseEndpoint *endpoint = CFdbContext::getInstance()->getEndpoint(mEpid);
        session = endpoint ? endpoint->preferredPeer() : 0;
        if (session)
        {
            mFlag &= ~MSG_FLAG_ENDPOINT;
            mSid = session->sid();
        }
    }
    else
    {
        session = CFdbContext::getInstance()->getSession(mSid);
    }
    return session;
}

bool CFdbMessage::deserialize(CFdbBasePayload &payload) const
{
    if (!mBuffer || notPbEncoded())
    {
        return false;
    }
    return deserializePb(payload
                         , mBuffer + mOffset + mPrefixSize + mHeadSize
                         , mPayloadSize);
}

bool CFdbMessage::deserializePb(CFdbBasePayload &payload, void *buffer, int32_t size)
{
    bool ret;
    if (!size)
    {
        ret = true;
    }
    else if (buffer)
    {
        try
        {
            //Assign ArrayInputStream with enough memory
            google::protobuf::io::ArrayInputStream ais(buffer, size);
            google::protobuf::io::CodedInputStream coded_input(&ais);
            google::protobuf::io::CodedInputStream::Limit msgLimit = coded_input.PushLimit(size);
            // De-Serialize
            ret = payload.ParseFromCodedStream(&coded_input);
            //Once the embedded message has been parsed, PopLimit() is called to undo the limit
            coded_input.PopLimit(msgLimit);
        }
        catch (...)
        {
            ret = false;
        }
    }
    else
    {
        ret = false;
    }
    
    return ret;
}

const char *CFdbMessage::getMsgTypeName(NFdbBase::wrapper::FdbMessageType type)
{
    static const char *type_name[] = {"Unknown"
                                    , "Request"
                                    , "Reply"
                                    , "Subscribe"
                                    , "Broadcast"
                                    , "SidebandRequest"
                                    , "SidebandReply"
                                    , "Status"};
    if (type >= NFdbBase::MT_MAX)
    {
        return 0;
    }
    return type_name[type];
}

void CFdbMessage::setLogData(const char *log_data)
{
    if (mStringData)
    {
        delete mStringData;
        mStringData = 0;
    }

    if (log_data)
    {
        mStringData = new std::string(log_data);
        mFlag |= MSG_FLAG_ENABLE_LOG;
    }
}

void CFdbMessage::setLogData(std::string *log_data)
{
    if (mStringData)
    {
        delete mStringData;
        mStringData = 0;
    }

    mStringData = log_data;
}


bool CFdbMessage::isSubscribe()
{
    return (mType == NFdbBase::MT_SUBSCRIBE_REQ) && (mCode == FDB_CODE_SUBSCRIBE);
}

void CFdbMessage::checkLogEnabled(const CFdbBaseObject *object, bool lock)
{
    if (!(mFlag & MSG_FLAG_ENABLE_LOG))
    {
        CLogProducer *logger = CFdbContext::getInstance()->getLogger();
        if (logger && logger->checkLogEnabled(this, object->endpoint(), lock))
        {
            mFlag |= MSG_FLAG_ENABLE_LOG;
        }
    }
}

CFdbDebugMsg::CFdbDebugMsg(FdbMsgCode_t code, EFdbMessageEncoding enc)
    : CFdbMessage(code, enc)
    , mSendTime(0)
    , mArriveTime(0)
    , mReplyTime(0)
    , mReceiveTime(0)
{
    mFlag |= MSG_FLAG_DEBUG;
}

CFdbDebugMsg::CFdbDebugMsg(FdbMsgCode_t code
                         , CFdbBaseObject *obj
                         , FdbSessionId_t alt_receiver
                         , EFdbMessageEncoding enc)
    : CFdbMessage(code, obj, alt_receiver, enc)
    , mSendTime(0)
    , mArriveTime(0)
    , mReplyTime(0)
    , mReceiveTime(0)
{
    mFlag |= MSG_FLAG_DEBUG;
}

CFdbDebugMsg::CFdbDebugMsg(NFdbBase::FdbMessageHeader &head
                           , CFdbMsgPrefix &prefix
                           , uint8_t *buffer
                           , FdbSessionId_t sid)
    : CFdbMessage(head, prefix, buffer, sid)
    , mSendTime(0)
    , mArriveTime(0)
    , mReplyTime(0)
    , mReceiveTime(0)
{
}

CFdbDebugMsg:: CFdbDebugMsg(FdbMsgCode_t code
                          , CFdbMessage *msg
                          , EFdbMessageEncoding enc)
    : CFdbMessage(code, msg, enc)
{
    mFlag |= MSG_FLAG_DEBUG;
}

void CFdbDebugMsg::encodeDebugInfo(NFdbBase::FdbMessageHeader &msg_hdr, CFdbSession *session)
{
    switch (msg_hdr.type())
    {
        case NFdbBase::MT_REPLY:
        case NFdbBase::MT_STATUS:
            msg_hdr.set_send_or_arrive_time(mArriveTime);
            msg_hdr.set_reply_time(CNanoTimer::getNanoSecTimer());
            break;
        case NFdbBase::MT_REQUEST:
        case NFdbBase::MT_SUBSCRIBE_REQ:
        case NFdbBase::MT_BROADCAST:
            mSendTime = CNanoTimer::getNanoSecTimer();
            msg_hdr.set_send_or_arrive_time(mSendTime);
            break;
        default:
            break;
    }
}

void CFdbDebugMsg::decodeDebugInfo(NFdbBase::FdbMessageHeader &msg_hdr, CFdbSession *session)
{
    switch (msg_hdr.type())
    {
        case NFdbBase::MT_REPLY:
        case NFdbBase::MT_STATUS:
            if (msg_hdr.has_send_or_arrive_time())
            {
                mArriveTime = msg_hdr.send_or_arrive_time();
            }
            if (msg_hdr.has_reply_time())
            {
                mReplyTime = msg_hdr.reply_time();
            }
            mReceiveTime = CNanoTimer::getNanoSecTimer();
            break;
        case NFdbBase::MT_REQUEST:
        case NFdbBase::MT_SUBSCRIBE_REQ:
        case NFdbBase::MT_BROADCAST:
            mArriveTime = CNanoTimer::getNanoSecTimer();
            if (msg_hdr.has_send_or_arrive_time())
            {
                mSendTime = msg_hdr.send_or_arrive_time();
            }
            break;
        default:
            break;
    }
}

void CFdbDebugMsg::metadata(CFdbMsgMetadata &metadata)
{
    metadata.mSendTime = mSendTime;
    metadata.mArriveTime = mArriveTime;
    metadata.mReplyTime = mReplyTime;
    metadata.mReceiveTime = mReceiveTime;
}

CFdbBroadcastMsg::CFdbBroadcastMsg(FdbMsgCode_t code
                                 , CFdbBaseObject *obj
                                 , const char *filter
                                 , FdbSessionId_t alt_sid
                                 , FdbObjectId_t alt_oid
                                 , EFdbMessageEncoding enc)
    : _CBaseMessage(code, obj, FDB_INVALID_ID, enc)
{
    if (filter)
    {
        mFilter = filter;
    }

    if (isValidFdbId(alt_sid))
    {
        mSid = alt_sid;
        mFlag &= ~MSG_FLAG_ENDPOINT;
        mSenderName = obj->name();
    }
    else
    {
        mEpid = obj->epid();
        mFlag |= MSG_FLAG_ENDPOINT;
    }
    if (isValidFdbId(alt_oid))
    {
        mOid = alt_oid;
    }
    mType = NFdbBase::MT_BROADCAST;
}

CFdbBroadcastMsg::CFdbBroadcastMsg(FdbMsgCode_t code
                                 , CFdbMessage *msg
                                 , const char *filter
                                 , EFdbMessageEncoding enc)
    : _CBaseMessage(code, msg, enc)
{
    if (filter)
    {
        mFilter = filter;
    }
    mFlag |= msg->mFlag & MSG_FLAG_MANUAL_UPDATE;

    mType = NFdbBase::MT_BROADCAST;
}

bool CFdbMessage::invokeSideband(int32_t timeout)
{
    CBaseJob::Ptr msg_ref(this);
    mType = NFdbBase::MT_SIDEBAND_REQUEST;
    return submit(msg_ref, 0, timeout);
}

bool CFdbMessage::sendSideband()
{
    CBaseJob::Ptr msg_ref(this);
    mType = NFdbBase::MT_SIDEBAND_REQUEST;
    return submit(msg_ref, FDB_MSG_TX_NO_REPLY, -1);
}


bool CFdbMessage::replySideband(CBaseJob::Ptr &msg_ref,
                                const CFdbBasePayload &data)
{
    CFdbMessage *fdb_msg = castToMessage<CFdbMessage *>(msg_ref);
    return fdb_msg->feedback(msg_ref, data, NFdbBase::MT_SIDEBAND_REPLY);
}