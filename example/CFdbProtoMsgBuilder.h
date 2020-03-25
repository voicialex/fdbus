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

#ifndef __IFDBPROTOMSGBUILDER_H__ 
#define __IFDBPROTOMSGBUILDER_H__

#include <common_base/IFdbMsgBuilder.h>
namespace google{namespace protobuf {class MessageLite;}}
typedef ::google::protobuf::MessageLite CFdbProtoMessage;

class CFdbProtoMsgBuilder : public IFdbMsgBuilder
{
public:
    CFdbProtoMsgBuilder(const CFdbProtoMessage &message);
    int32_t build();
    void toBuffer(uint8_t *buffer, int32_t size);
    bool toString(std::string *msg_txt) const;
    
private:
    const CFdbProtoMessage &mMessage;
};

class CFdbProtoMsgParser : public IFdbMsgParser
{
public:
    CFdbProtoMsgParser(CFdbProtoMessage &message);
    bool parse(const uint8_t *buffer, int32_t size);

private:
    CFdbProtoMessage &mMessage;
};

#endif
