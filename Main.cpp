#include <cassert>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

#include <arpa/inet.h>
#include <sys/select.h>

#include "DirectPlayMessage.hpp"
#include "IniFile.hpp"
#include "Socket.hpp"

static std::u16string convertUTF8ToUCS2(std::string_view u8)
{
    std::u16string ret;
    ret.reserve(u8.size()); // pessimistic

    auto end = u8.end();

    for(auto it = u8.begin(); it != end; ++it)
    {
        auto c = static_cast<unsigned>(*it);
        if(c < 0x80)
            ret += c;
        else if((c & 0xE0) == 0xC0)
        {
            // two byte seq
            if(++it == end)
                break;

            auto c1 = static_cast<unsigned>(*it);
            ret += static_cast<char16_t>((c & 0x1F) << 6 | (c1 & 0x3F));
        }
        else if((c & 0xF0) == 0xE0)
        {
            // three bytes
            if(++it == end)
                break;

            auto c1 = static_cast<unsigned>(*it);
            if(++it == end)
                break;

            auto c2 = static_cast<unsigned>(*it);

            ret += static_cast<char16_t>((c & 0x0F) << 12 | (c1 & 0x3F) << 6 | (c2 & 0x3F));
        }
        else
            break;
    }

    return ret;
}

static std::string convertUCS2ToUTF8(std::u16string_view u16)
{
    std::string ret;
    ret.reserve(u16.length()); // optimistic

    for(auto &c : u16)
    {
        assert(c < 0xD800 || c >= 0xE000); // not UTF-16, no surrogates

        if(c <= 0x7F)
            ret += static_cast<char>(c);
        else if(c <= 0x7FF)
        {
            ret += static_cast<char>(0xC0 | c >> 6);
            ret += static_cast<char>(0x80 | (c & 0x3F));
        }
        else // <= 0xFFFF
        {
            ret += static_cast<char>(0xE0 | c >> 12);
            ret += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            ret += static_cast<char>(0x80 | (c & 0x3F));
        }
    }

    return ret;
}

class Player final
{
public:
    Player(uint32_t id, uint32_t systemPlayerId, uint32_t flags) : id(id), flags(flags), systemPlayerId(systemPlayerId)
    {
    }

    ~Player()
    {
        delete[] serviceProviderData;
        delete[] data;
    }

    uint32_t getId() const
    {
        return id;
    }

    uint32_t getSystemPlayerId() const
    {
        return systemPlayerId;
    }

    uint32_t getFlags() const
    {
        return flags;
    }

    void setShortName(std::string name)
    {
        shortName = std::move(name);
    }

    void setLongName(std::string name)
    {
        longName = std::move(name);
    }

    uint32_t getServiceProviderDataLen() const
    {
        return serviceProviderDataLen;
    }

    const uint8_t *getServiceProviderData() const
    {
        return serviceProviderData;
    }

    void setServiceProviderData(const uint8_t *data, uint32_t len)
    {
        delete[] serviceProviderData;

        serviceProviderData = new uint8_t[len];
        serviceProviderDataLen = len;

        memcpy(serviceProviderData, data, len);
    }

private:
    uint32_t id;
    uint32_t flags;

    uint32_t systemPlayerId;

    std::string shortName, longName;

    uint8_t *serviceProviderData = nullptr;
    uint32_t serviceProviderDataLen = 0;

    uint8_t *data = nullptr;
    uint32_t dataLen;
};

class Session final
{
public:
    Session(std::string name, uint8_t *appGUID, uint32_t flags) : name(std::move(name)), flags(flags)
    {
        memset(guid, 1, 16); // TODO: generate valid guid
        memcpy(this->appGUID, appGUID, 16);

        startTime = std::chrono::steady_clock::now();
    }

    const uint8_t *getGUID() const
    {
        return guid;
    }

    const uint8_t *getAppGUID() const
    {
        return appGUID;
    }

    const std::string &getName() const
    {
        return name;
    }

    uint32_t getFlags() const
    {
        return flags;
    }

    uint32_t getMaxPlayers() const
    {
        return maxPlayers;
    }

    uint32_t getCurrentPlayers() const
    {
        // count non-system players
        uint32_t ret = 0;
        for(auto &player : players)
        {
            if(!(player.second.getFlags() & DPPlayer_System))
                ret++;
        }
        return ret;
    }

    uint32_t getIdXor() const
    {
        return idXor;
    }

    uint32_t adjustId(uint32_t id) const
    {
        return id ^ idXor;
    }

    uint32_t getTickCount() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
    }

    Player &createNewSystemPlayer(uint32_t flags = 0)
    {
        auto newId = allocPlayerId();
        return players.emplace(newId, Player{newId, newId, flags | DPPlayer_System}).first->second;
    }

    Player &createNewPlayer(uint32_t systemPlayerId, uint32_t flags = 0)
    {
        auto newId = allocPlayerId();
        return players.emplace(newId, Player{newId, systemPlayerId, flags}).first->second;
    }

    void deletePlayer(uint32_t id)
    {
        auto it = players.find(id);

        if(it == players.end())
            return;

        if(it->second.getFlags() & DPPlayer_System)
        {
            // system player, remove all non-system players
            // this also includes the system player as its system player is itself...
            for(auto it2 = players.begin(); it2 != players.end();)
            {
                if(it2->second.getSystemPlayerId() == id)
                    it2 = players.erase(it2);
                else
                    ++it2;
            }
        }
        else
            players.erase(it);
    }

    Player *getPlayer(uint32_t id)
    {
        auto it = players.find(id);

        if(it != players.end())
            return &it->second;

        return nullptr;
    }

    const std::map<uint32_t, Player> &getPlayers() const
    {
        return players;
    }

private:
    uint32_t allocPlayerId()
    {
        // TODO: less bad id alloc
        // should be "a zero-based value not shared by an existing identifier" | "a zero-based value that is incremented to provide uniqueness" << 16
        uint32_t newId = players.size() | idUnique << 16;
        
        while(players.find(newId) != players.end())
            newId++;

        return newId;   
    }

    uint8_t guid[16];
    uint8_t appGUID[16];

    std::string name;
    uint32_t flags;

    uint32_t maxPlayers = 10; // TODO
    uint32_t idXor = 0; // TODO: init
    uint32_t idUnique = 1; // TODO: incremented at some point

    std::chrono::steady_clock::time_point startTime;

    std::map<uint32_t, Player> players;
};

class Client final
{
public:
    Client(Session &session, std::string address, int outgoingPort) : session(session), address(std::move(address)), outgoingPort(outgoingPort), tcpIncoming(SocketType::TCP), tcpOutgoing(SocketType::TCP), udpSocket(SocketType::UDP)
    {
    }

    Client(Client &&other) : Client(other.session, other.address, other.outgoingPort)
    {
        *this = std::move(other);
    }

    ~Client()
    {
        if(systemPlayerId != ~0u)
            session.deletePlayer(systemPlayerId);
    }

    Client &operator=(Client &&other)
    {
        if(this != &other)
        {
            session = other.session;

            address = std::move(other.address);
    
            outgoingPort = other.outgoingPort;
            tcpIncoming = std::move(other.tcpIncoming);
            tcpOutgoing = std::move(other.tcpOutgoing);
    
            systemPlayerId = other.systemPlayerId;

            other.systemPlayerId = -1;
        }

        return *this;
    }

    bool handleDPlayPacket(const uint8_t *data, size_t &len)
    {
        if(len < sizeof(DPSPMessageHeader))
        {
            // not enough data for a header
            len = 0;
            return false;
        }

        // something something byte-order something
        auto header = reinterpret_cast<const DPSPMessageHeader *>(data);

        auto packetSize = header->sizeToken & 0xFFFFF;
        //auto token = header->sizeToken >> 20;

        if(len < packetSize)
        {
            // not enough data
            len = packetSize;
            return false;
        }

        len = packetSize;

        if(memcmp(header->signature, "play", 4) != 0)
            return false;

        // only handle dx9
        if(header->version != 14)
            return false;

        return handleDPlayCommand(header->command, data + sizeof(DPSPMessageHeader), len - sizeof(DPSPMessageHeader));
    }

    void handleUDPRead()
    {
        // this is for data received after joining a session
        uint8_t buf[2048];
        int len = udpSocket.recv(buf, sizeof(buf));
        
        // zero for disconnected is a tcp thing...?
        if(len <= 0)
            return;

        // assume we're using the "reliable protocol"

        if(len < 6)
        {
            std::cerr << "short frame? " << len << "\n";
            return;
        }

        auto ptr = buf;
        auto end = ptr + len;

        // variable length ids
        uint16_t fromId, toId;

        fromId = *ptr++;

        if(fromId & 0x80)
            fromId = (fromId & 0x7F) | (*ptr++) << 7;

        if(fromId & 0x4000)
            fromId = (fromId & 0x3FF) | (*ptr++) << 14;

        toId = *ptr++;

        if(toId & 0x80)
            toId = (toId & 0x7F) | (*ptr++) << 7;

        if(toId & 0x4000)
            toId = (toId & 0x3FF) | (*ptr++) << 14;

        size_t idLen = ptr - buf; // not counted in total data

        // make sure we have enough for the rest of the header
        if(end - ptr < 4)
        {
            std::cerr << "short frame? " << len << "\n";
            return;
        }

        uint8_t flags = *ptr++;
        uint8_t messageId = *ptr++;
        uint8_t sequence = *ptr++;
        uint8_t serial = *ptr++;

        if(flags & DPRPFrame_Extended)
        {
            std::cerr << "ext flags\n";
            return;
        }

        size_t dataLen = end - ptr;

        dataReceived += len - idLen;

        // validate player indices
        if(toId != 0)
        {
            std::cerr << "frame to " << toId << "\n";
            return;
        }

        // check message id
        // if no ongoing/completed recv, first recv = message id
        // if message id outside first ongoing/completed recv -> +23, discard
        // if not receiving this id, add to list
        // if already received, send ack (prev one got lost)

        // send nack if unexpected sequence
        // ... or don't as sequence numbers are always 1 and nacks are unimplemented?

        if(flags & DPRPFrame_Ack)
        {
            std::cout << "rp ack" << std::endl;
        }
        else if((flags & DPRPFrame_Start) && (flags & DPRPFrame_End))
        {
            // single frame message, avoid all the copying
            handleCompletedRPMessage(ptr, dataLen);
        }
        else
        {
            // basic message assembly
            if(flags & DPRPFrame_Start)
            {
                if(currentMessageId != -1)
                    std::cerr << "rp multi msg\n";

                currentMessageId = messageId;
                nextMessageSequence = sequence + 1;

                // copy initial data
                messageBuffer.resize(dataLen);
                memcpy(messageBuffer.data(), ptr, dataLen);
            }
            else if(sequence == nextMessageSequence)
            {
                // append
                auto offset = messageBuffer.size();
                messageBuffer.resize(offset + dataLen);
                memcpy(messageBuffer.data() + offset, ptr, dataLen);

                nextMessageSequence++;
            }
            else
            {
                std::cerr << "rp seq err\n";
                return;
            }
            
            if(flags & DPRPFrame_End)
            {
                handleCompletedRPMessage(messageBuffer.data(), messageBuffer.size());
                currentMessageId = -1;
            }
        }

        // send ack if requested or end of message
        if(flags & (DPRPFrame_End | DPRPFrame_SendAck))
        {
            // send ack
            uint8_t replyFlags = DPRPFrame_Ack | (flags & DPRPFrame_Reliable); // reliably ack a reliable packet
            auto replySize = getRPHeaderSize(toId, fromId) + 8;
            auto replyBuf = new uint8_t[replySize];

            auto ptr = fillRPHeader(replyBuf, toId, fromId, replyFlags, messageId, sequence, serial);

            *reinterpret_cast<uint32_t *>(ptr) = dataReceived;
            *reinterpret_cast<uint32_t *>(ptr + 4) = session.getTickCount();

            if(!udpSocket.send(replyBuf, replySize))
            {
                std::cerr << "Failed to send ack!\n";
            }

            delete[] replyBuf;
        }
    }

    Socket &getTCPIncomingSocket()
    {
        return tcpIncoming;
    }

    void setTCPIncomingSocket(Socket &&socket)
    {
        tcpIncoming = std::move(socket);
    }

    Socket &getUDPSocket()
    {
        return udpSocket;
    }

private:
    bool handleDPlayCommand(DPSPCommand command, const uint8_t *data, size_t len)
    {
        // data/len don't include header here
        switch(command)
        {
            case DPSPCommand::EnumSessions:
            {
                auto cmd = reinterpret_cast<const DPSPMessageEnumSessions *>(data);
                // TODO: password?
                std::cout << "enum sessions " << cmd->passwordOffset << " " << cmd->flags << std::endl;

                // don't reply if app mismatch
                if(memcmp(cmd->applicationGUID, session.getAppGUID(), 16) != 0)
                {
                    std::cerr << "app guid mismatch\n";
                    return true;
                }

                // reply
                if(checkOutgoingSocket())
                {
                    auto sessionName = convertUTF8ToUCS2(session.getName());
                    size_t replySize = sizeof(DPSPMessageHeader) + sizeof(DPSPMessageEnumSessionsReply) + (sessionName.length() + 1) * 2;

                    auto replyBuffer = new uint8_t[replySize];
                    auto header = reinterpret_cast<DPSPMessageHeader *>(replyBuffer);
                    auto replyMessage = reinterpret_cast<DPSPMessageEnumSessionsReply *>(replyBuffer + sizeof(DPSPMessageHeader));

                    fillOutgoingHeader(header, replySize, DPSPCommand::EnumSessionsReply);
                    fillSessionDesc(&replyMessage->sessionDescription);

                    replyMessage->nameOffset = sizeof(DPSPMessageEnumSessionsReply) + 8;

                    // copy the name
                    memcpy(replyBuffer + sizeof(DPSPMessageHeader) + sizeof(DPSPMessageEnumSessionsReply), sessionName.data(), sessionName.length() * 2);
                    replyBuffer[replySize - 2] = 0;
                    replyBuffer[replySize - 1] = 0;

                    if(!tcpOutgoing.sendAll(replyBuffer, replySize))
                    {
                        std::cerr << "Failed to send enum sessions reply!\n";
                    }

                    delete[] replyBuffer;
                }
                return true;
            }

            case DPSPCommand::RequestPlayerId:
            {
                auto cmd = reinterpret_cast<const DPSPMessageRequestPlayerId *>(data);

                bool isSystem = cmd->flags & RequestPlayerId_System;

                if(isSystem && systemPlayerId != ~0u)
                {
                    std::cerr << "client requesting system player id when they already have one\n";
                    return true;
                }

                std::cout << "req player id " << isSystem << std::endl;

                auto &newPlayer = isSystem ? session.createNewSystemPlayer() : session.createNewPlayer(systemPlayerId);
                
                if(isSystem)
                    systemPlayerId = newPlayer.getId();

                if(checkOutgoingSocket())
                {
                    size_t replySize = sizeof(DPSPMessageHeader) + sizeof(DPSPMessageRequestPlayerReply);

                    auto replyBuffer = new uint8_t[replySize];
                    auto header = reinterpret_cast<DPSPMessageHeader *>(replyBuffer);
                    auto replyMessage = reinterpret_cast<DPSPMessageRequestPlayerReply *>(replyBuffer + sizeof(DPSPMessageHeader));

                    fillOutgoingHeader(header, replySize, DPSPCommand::RequestPlayerReply);

                    // zero out security info
                    memset(replyMessage, 0, sizeof(DPSPMessageRequestPlayerReply));

                    replyMessage->id = session.adjustId(newPlayer.getId());

                    if(!tcpOutgoing.sendAll(replyBuffer, replySize))
                    {
                        std::cerr << "Failed to send request id reply!\n";
                    }
                    delete[] replyBuffer;
                }
                return true;
            }

            case DPSPCommand::CreatePlayer:
            {
                auto cmd = reinterpret_cast<const DPSPMessageCreatePlayer *>(data);
                auto ptr = data + cmd->createOffset - 8;
                auto playerInfo = reinterpret_cast<const DPPackedPlayer *>(ptr);

                ptr += sizeof(DPPackedPlayer);

                auto player = session.getPlayer(session.adjustId(playerInfo->playerId));

                if(!player)
                {
                    std::cerr << "player not found for create!\n";
                    return true;
                }

                // short name
                std::u16string_view shortName(reinterpret_cast<const char16_t *>(ptr), playerInfo->shortNameLength / 2);
                ptr += playerInfo->shortNameLength;
                player->setShortName(convertUCS2ToUTF8(shortName));

                // long name
                std::u16string_view longName(reinterpret_cast<const char16_t *>(ptr), playerInfo->longNameLength / 2);
                ptr += playerInfo->longNameLength;
                player->setShortName(convertUCS2ToUTF8(longName));

                // service provider data
                if(playerInfo->serviceProviderDataSize)
                {
                    player->setServiceProviderData(ptr, playerInfo->serviceProviderDataSize);
                    ptr += playerInfo->serviceProviderDataSize;
                }

                // no reply

                // is this the right place to open the socket?
                // it's the last thing sent before switching to UDP...
                // (and we're connecting the socket, it's only used to send to this client)
                if(!udpSocket.connect(address.c_str(), outgoingPort, outgoingPort))
                    std::cerr << "failed to connect UDP socket\n";

                return true;
            }

            case DPSPCommand::AddForwardRequest:
            {
                auto cmd = reinterpret_cast<const DPSPMessageAddForwardRequest *>(data);
                auto ptr = data + cmd->createOffset - 8;
                auto playerInfo = reinterpret_cast<const DPPackedPlayer *>(ptr);
                auto password = std::u16string_view(reinterpret_cast<const char16_t *>(data + cmd->createOffset - 8 + playerInfo->size));
                auto tickCount = *reinterpret_cast<const uint32_t *>(data + cmd->createOffset - 8 + playerInfo->size + (password.length() + 1) * 2);

                ptr += sizeof(DPPackedPlayer);

                auto player = session.getPlayer(session.adjustId(playerInfo->playerId));

                if(!player)
                {
                    std::cerr << "player not found for add fwd!\n";
                    return true;
                }

                std::u16string_view shortName(reinterpret_cast<const char16_t *>(ptr), playerInfo->shortNameLength / 2);
                ptr += playerInfo->shortNameLength;
                player->setShortName(convertUCS2ToUTF8(shortName));

                // long name
                std::u16string_view longName(reinterpret_cast<const char16_t *>(ptr), playerInfo->longNameLength / 2);
                ptr += playerInfo->longNameLength;
                player->setShortName(convertUCS2ToUTF8(longName));

                // service provider data
                if(playerInfo->serviceProviderDataSize)
                {
                    player->setServiceProviderData(ptr, playerInfo->serviceProviderDataSize);
                    ptr += playerInfo->serviceProviderDataSize;
                }

                // TODO: player data

                if(checkOutgoingSocket())
                {
                    // if session flags & DPSession_ServerPlayerOnly return EnumPlayersReply instead

                    // this is a big one
                    size_t replySize = sizeof(DPSPMessageHeader) + sizeof(DPSPMessageSuperEnumPlayersReply);

                    // session description + name
                    auto sessionName = convertUTF8ToUCS2(session.getName());
                    replySize += sizeof(DPSessionDesc2) + (sessionName.length() + 1) * 2;

                    auto &players = session.getPlayers();
                    replySize += sizeof(DPSuperPackedPlayer) * players.size();

                    for(auto &player : players)
                    {
                        // TODO: + names and data
                        auto spDataLen = player.second.getServiceProviderDataLen();
                        if(spDataLen)
                            replySize += spDataLen + 1; // we only support sockets so this will always be 32
                    }

                    auto replyBuffer = new uint8_t[replySize];
                    auto header = reinterpret_cast<DPSPMessageHeader *>(replyBuffer);
                    auto ptr = replyBuffer + sizeof(DPSPMessageHeader);
                    auto replyMessage = reinterpret_cast<DPSPMessageSuperEnumPlayersReply *>(ptr);

                    fillOutgoingHeader(header, replySize, DPSPCommand::SuperEnumPlayersReply);

                    replyMessage->playerCount = players.size();
                    replyMessage->groupCount = 0;
                    replyMessage->shortcutCount = 0;
                    replyMessage->passwordOffset = 0;

                    // session
                    ptr += sizeof(DPSPMessageSuperEnumPlayersReply);
                    replyMessage->descriptionOffset = ptr - replyBuffer - 20;
                    auto sessionDesc = reinterpret_cast<DPSessionDesc2 *>(ptr);
                    fillSessionDesc(sessionDesc);

                    // session name
                    ptr += sizeof(DPSessionDesc2);
                    replyMessage->nameOffset = ptr - replyBuffer - 20;
                    memcpy(ptr, sessionName.data(), sessionName.length() * 2);

                    // null terminate
                    ptr += sessionName.length() * 2;
                    *ptr++ = 0;
                    *ptr++ = 0;

                    // players
                    replyMessage->packedOffset = ptr - replyBuffer - 20;
                    for(auto &player : players)
                    {
                        auto superPlayer = reinterpret_cast<DPSuperPackedPlayer *>(ptr);

                        superPlayer->size = 16;
                        superPlayer->flags = player.second.getFlags();
                        superPlayer->id = session.adjustId(player.first);
                        superPlayer->playerInfoMask = 0;

                        if(superPlayer->flags & DPPlayer_System)
                            superPlayer->versionOrSystemPlayerId = 14; // version
                        else
                            superPlayer->versionOrSystemPlayerId = player.second.getSystemPlayerId();

                        ptr += sizeof(DPSuperPackedPlayer);
                        // TODO: names + data

                        // service provider data
                        auto spDataLen = player.second.getServiceProviderDataLen();
                        if(spDataLen)
                        {
                            superPlayer->playerInfoMask |= 1 << DPSuperPlayer_ServiceProviderDataShift;
                            *ptr++ = spDataLen;

                            memcpy(ptr, player.second.getServiceProviderData(), spDataLen);

                            ptr += spDataLen;
                        }
                    }

                    if(!tcpOutgoing.sendAll(replyBuffer, replySize))
                    {
                        std::cerr << "Failed to send add forward reply!\n";
                    }
                    delete[] replyBuffer;
                }

                return true;
            }

            case DPSPCommand::Packet:
            {
                auto cmd = reinterpret_cast<const DPSPMessagePacket *>(data);
                auto packetData = data + sizeof(DPSPMessagePacket);

                if(cmd->totalPackets == 1)
                {
                    // don't have the optional fields
                    auto headerSize = sizeof(DPSPMessageHeader) - offsetof(DPSPMessageHeader, signature);
                    DPSPMessageHeader packetHeader;
                    memcpy(&packetHeader.signature, packetData, headerSize);

                    if(packetHeader.version != 14 || memcmp(packetHeader.signature, "play", 4) != 0)
                    {
                        std::cerr << "bad nested packet\n";
                    }
                    else
                        return handleDPlayCommand(packetHeader.command, packetData + headerSize, cmd->dataSize - headerSize);
                }
                else
                {
                    //FIXME: actually re-assemble the packet
                    std::cerr << "packet " << cmd->packetIndex << "/" << cmd->totalPackets << "\n";
                }
                return true;
            }

            default:
                std::cerr << "unhandled dplay cmd " << int(command) << "(size " << len << ")\n";
        }

        return false;
    }

    void handleCompletedRPMessage(const uint8_t *data, size_t len)
    {
        if(memcmp(data, "play", 4) == 0)
        {
            // the data is a dplay message
            auto headerSize = sizeof(DPSPMessageHeader) - offsetof(DPSPMessageHeader, signature);
            DPSPMessageHeader packetHeader;
            memcpy(&packetHeader.signature, data, headerSize);
            
            handleDPlayCommand(packetHeader.command, data + headerSize, len - headerSize);
            return;
        }

        std::cout << "rp msg len " << len << std::endl;
        std::cout << "\t";

        for(size_t i = 0; i < len; i++)
        {
            auto b = data[i];
            auto hi = b >> 4;
            auto lo = b & 0xF;

            std::cout << static_cast<char>(hi > 9 ? 'A' + hi - 10 : '0' + hi)
                      << static_cast<char>(lo > 9 ? 'A' + lo - 10 : '0' + lo)
                      << " ";
        }

        std::cout << std::endl;
    }

    bool checkOutgoingSocket()
    {
        // TODO: add an isConnected? (or some more accurate name for an fd existing)
        if(tcpOutgoing.getFd() != -1)
            return true;

        std::cout << "Open outgoing to " << address << std::endl;

        if(!tcpOutgoing.connect(address.c_str(), outgoingPort))
        {
            std::cerr << "failed to open outgoing connection to " << address << "\n";
            return false;
        }

        return true;
    }

    void fillOutgoingHeader(DPSPMessageHeader *header, size_t size, DPSPCommand command)
    {
        header->sizeToken = size | 0xFAB << 20;

        header->sockaddr.family = 2;
        header->sockaddr.port = htons(outgoingPort);
        header->sockaddr.addr = 0;
        memset(header->sockaddr.padding, 0, 8);

        memcpy(header->signature, "play", 4);
        header->command = command;
        header->version = 14;
    }

    void fillSessionDesc(DPSessionDesc2 *desc)
    {
        desc->size = sizeof(DPSessionDesc2);
        desc->flags = session.getFlags();
        memcpy(desc->instanceGUID, session.getGUID(), 16);
        memcpy(desc->applicationGUID, session.getAppGUID(), 16);
        desc->maxPlayers = session.getMaxPlayers();
        desc->currentPlayerCount = session.getCurrentPlayers();

        desc->sessionName = 0;
        desc->password = 0;

        desc->reserved1 = session.getIdXor();
        desc->reserved2 = 0;

        desc->applicationDefined1 = 0;
        desc->applicationDefined2 = 0;
        desc->applicationDefined3 = 0;
        desc->applicationDefined4 = 0;
    }

    // reliable protocol
    size_t getRPHeaderSize(uint16_t from, uint16_t to)
    {
        size_t ret = 4;

        if(to < 128)
            ret += 1;
        else if(to < 16384)
            ret += 2;
        else
            ret += 3;

        if(from < 128)
            ret += 1;
        else if(from < 16384)
            ret += 2;
        else
            ret += 3;

        return ret;
    }

    uint8_t *fillRPHeader(uint8_t *data, uint16_t from, uint16_t to, uint8_t flags, uint8_t messageId, uint8_t sequence, uint8_t serial)
    {
        // from
        if(from < 128)
            *data++ = from & 0x7F;
        else if(from < 14384)
        {
            *data++ = (from & 0x7F) | 0x80;
            *data++ = from >> 7;
        }
        else
        {
            *data++ = (from & 0x7F) | 0x80;
            *data++ = ((from >> 7) & 0x7F) | 0x80;
            *data++ = from >> 14;
        }

        // to
        if(to < 128)
            *data++ = to & 0x7F;
        else if(to < 14384)
        {
            *data++ = (to & 0x7F) | 0x80;
            *data++ = to >> 7;
        }
        else
        {
            *data++ = (to & 0x7F) | 0x80;
            *data++ = ((to >> 7) & 0x7F) | 0x80;
            *data++ = to >> 14;
        }

        // flags
        *data++ = flags;

        // nack has ext flags here (but ext flags aren't implemented)
        // ... and neither are nacks

        *data++ = messageId;
        *data++ = sequence;
        *data++ = serial; // not for nack

        return data;
    }

    Session &session;

    std::string address;

    int outgoingPort;
    Socket tcpIncoming, tcpOutgoing;

    Socket udpSocket;

    uint32_t systemPlayerId = ~0u;

    // "reliable protocol" related
    uint32_t dataReceived = 0;

    // TODO: docs suggest that multiple messages can be in flight at once
    int currentMessageId = -1;
    uint8_t nextMessageSequence = 0;
    std::vector<uint8_t> messageBuffer;
};

int main(int argc, char *argv[])
{
    // get config
    IniFile config("./config.ini");

    auto port = config.getIntValue("Server", "Port");
    auto addr = config.getValue("Server", "ListenAddr");
    auto sessionName = config.getValue("Server", "SessionName");
    auto guid = config.getValue("Server", "AppGUID");

    if(!port || !addr || !sessionName || !guid)
    {
        std::cerr << "failed to get config from config.ini\n";
        std::cerr << "port: " << port.value_or(-1) << ", addr: " << addr.value_or("MISSING") 
                  << ", session name: " << sessionName.value_or("MISSING") << ", guid: " << guid.value_or("MISSING") << "\n";
        return 1;
    }

    // parse guid
    uint8_t appGUID[16];

    if(guid->length() != 36)
    {
        std::cerr << "invalid GUID " << *guid << "(" << guid->length() << ")\n";
        return 1;
    }

    auto start = guid->data();
    auto end = start + guid->length();
    for(int i = 0; i < 16; i++)
    {
        // skip separators
        if(*start == '-')
            start++;

        if(start + 2 > end)
            break;

        auto res = std::from_chars(start, start + 2, appGUID[i], 16);

        if(res.ec != std::errc{})
            break;
        
        start = res.ptr;
    }

    if(start != end)
    {
        std::cerr << "failed to pares GUID " << *guid << "\n";
        return 1;
    }

    std::cout << "starting server on " << *addr << ", port " << *port << ", app guid: " << *guid << ", session name: " << *sessionName << std::endl;

    // setup sockets
    Socket tcpListen(SocketType::TCP);
    Socket udpListen(SocketType::UDP);

    // annoying, but not as annoying as trying to pass a string_view to inet_pton
    std::string addrStr(*addr);

    // TODO: logging?
    if(!tcpListen.listen(addrStr.c_str(), *port))
    {
        return 1;
    }

    // directplay broadcast port
    if(!udpListen.bind(addrStr.c_str(), 47624))
    {
        return 1;
    }

    // copying values returned by game in a regular multiplayer session...
    uint32_t sessionFlags = /*DPSession_PingTimer |*/ DPSession_ReliableProtocol | DPSession_OptimiseLatency;
    Session session(std::string(*sessionName), appGUID, sessionFlags);

    // create local system player
    auto &localPlayer = session.createNewSystemPlayer(DPPlayer_NameServer | DPPlayer_SendingMachine);
  
    // set service provider data (2x sockaddr with addr=0.0.0.0)
    // these are the TCP and UDP ports
    DPSockaddrIn spData[2] = {};
    spData[0].family = spData[1].family = 2;
    spData[0].port = spData[1].port = htons(*port);
    localPlayer.setServiceProviderData(reinterpret_cast<uint8_t *>(spData), sizeof(spData));

    std::map<std::string, Client> clients;

    while(true)
    {
        // TODO: select wrapper
        fd_set fds;
        int maxFd = -1;
        FD_ZERO(&fds);

        auto addFd = [&fds, &maxFd](int fd)
        {
            FD_SET(fd, &fds);

            if(fd > maxFd)
                maxFd = fd;
        };

        addFd(tcpListen.getFd());
        addFd(udpListen.getFd());

        for(auto &client : clients)
        {
            int fd = client.second.getTCPIncomingSocket().getFd();
            if(fd != -1)
                addFd(fd);

            fd = client.second.getUDPSocket().getFd();
            if(fd != -1)
                addFd(fd);
        }

        int ready = select(maxFd + 1, &fds, nullptr, nullptr, nullptr);

        if(ready < 0)
        {} // ohno
        else if(ready == 0)
            continue;

        // check sockets
        if(FD_ISSET(tcpListen.getFd(), &fds))
        {
            SocketAddress addr;
            auto newSock = tcpListen.accept(&addr);

            if(newSock)
            {
                std::cout << "tcp accept " << addr.toString(true) << std::endl;

                auto key = addr.toString(); // assuming one client per ip, not sure we can do better...

                auto it = clients.find(key);

                if(it == clients.end())
                    it = clients.emplace(key, Client{session, key, *port}).first;

                it->second.setTCPIncomingSocket(std::move(newSock.value()));
            }
        }

        if(FD_ISSET(udpListen.getFd(), &fds))
        {
            uint8_t buf[2048];
            SocketAddress addr;
            int len = udpListen.recv(buf, sizeof(buf), &addr);
            std::cout << "udp recv " << len << " from " << addr.toString(true) << std::endl;

            // get client
            auto key = addr.toString();

            auto it = clients.find(key);

            if(it == clients.end())
                it = clients.emplace(key, Client{session, key, *port}).first;

            // parse directplay packet
            size_t parsedLen = len;
            it->second.handleDPlayPacket(buf, parsedLen);

            // should have one packet
            if(parsedLen != static_cast<size_t>(len))
                std::cerr << "udp packet size mismatch " << parsedLen << "/" << len << "\n";
        }

        // check client sockets
        for(auto it = clients.begin(); it != clients.end();)
        {
            auto &client = *it;
            int fd = client.second.getTCPIncomingSocket().getFd();
            if(fd != -1 && FD_ISSET(fd, &fds))
            {
                // TODO: move most of this to client
                auto &socket = client.second.getTCPIncomingSocket();
                uint8_t buf[2048];
                int len = socket.recv(buf, sizeof(buf));

                if(len == 0)
                {
                    // disconnect
                    std::cout << "tcp disconnect " << client.first << std::endl;
                    socket.close();

                    it = clients.erase(it);
                    continue;
                }
                else if(len > 0)
                {
                    std::cout << "tcp recv " << len << " from " << client.first << std::endl;

                    // FIXME: buffering
                    size_t parsedLen = len;
                    client.second.handleDPlayPacket(buf, parsedLen);

                    // FIXME: can have multiple packets
                    if(parsedLen != static_cast<size_t>(len))
                        std::cerr << "tcp need buf " << parsedLen << "/" << len << "\n";
                }
            }

            fd = client.second.getUDPSocket().getFd();
            if(fd != -1 && FD_ISSET(fd, &fds))
               client.second.handleUDPRead();

            ++it;
        }
    }

    return 0;
}
