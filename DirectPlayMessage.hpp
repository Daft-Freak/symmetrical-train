#pragma once

#include <cstdint>

// this is the same as the regular sockaddr_in...
struct DPSockaddrIn
{
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t padding[8];
};
static_assert(sizeof(DPSockaddrIn) == 16);

enum DPPlayerFlags
{
    DPPlayer_System         = 1 << 0,
    DPPlayer_NameServer     = 1 << 1,
    DPPlayer_InGroup        = 1 << 2,
    DPPlayer_SendingMachine = 1 << 3, // ignored
};

struct DPPackedPlayer
{
    uint32_t size;
    uint32_t flags;
    uint32_t playerId;
    uint32_t shortNameLength;
    uint32_t longNameLength;
    uint32_t serviceProviderDataSize;
    uint32_t playerDataSize;
    uint32_t numberOfPlayers;
    uint32_t systemPlayerId;
    uint32_t fixedSize; // must be 48
    uint32_t playerVersion;
    uint32_t parentId;

    // shortname, longname, serviceproviderdata, playerdata, playerids
};
static_assert(sizeof(DPPackedPlayer) == 48);

enum DPSuperPlayerInfoMask
{
    DPSuperPlayer_ShortName           = 1 << 0,
    DPSuperPlayer_LongName            = 1 << 1,
    DPSuperPlayer_ServiceProviderData = 3 << 2, // 1, 2, 4 bytes
    DPSuperPlayer_PlayerData          = 3 << 4, // 1, 2, 4 bytes
    DPSuperPlayer_PlayerCount         = 3 << 6, // 1, 2, 4 bytes
    DPSuperPlayer_ParentID            = 1 << 8,
    DPSuperPlayer_ShortcutCount       = 3 << 9, // 1, 2, 4 bytes
};

enum DPSuperPlayerInfoShift
{
    DPSuperPlayer_ServiceProviderDataShift = 2,
    DPSuperPlayer_PlayerDataShift          = 4,
    DPSuperPlayer_PlayerCountShift         = 6,
    DPSuperPlayer_ShortcutCountShift       = 9,
};

// like above, but has a cape
struct DPSuperPackedPlayer
{
    uint32_t size; // size of header, must be 16
    uint32_t flags;
    uint32_t id;
    uint32_t playerInfoMask;

    uint32_t versionOrSystemPlayerId;

    // shortname, longname, playerdata, serviceproviderdata, playerids, shortcutids (based on info mask)
};
static_assert(sizeof(DPSuperPackedPlayer) == 20);

struct DPSecurityDesc
{
    uint32_t size;
    uint32_t flags; // unused
    uint32_t sspiProvider; // ignored
    uint32_t capiProvider; // ignored
    uint32_t capiProviderType;
    uint32_t encryptionAlgorithm;
};
static_assert(sizeof(DPSecurityDesc) == 24);

enum DPSessionFlags
{
    DPSession_NoNewPlayers        = 1 << 0,
    DPSession_MigrateHost         = 1 << 2,
    DPSession_NoPlayerToFrom      = 1 << 3,
    DPSession_NoJoin              = 1 << 5,
    DPSession_PingTimer           = 1 << 6,
    DPSession_NoDataChange        = 1 << 7,
    DPSession_UserAuth            = 1 << 8,
    DPSession_Private             = 1 << 9,
    DPSession_PasswordRequired    = 1 << 10,
    DPSession_RouteThroughHost    = 1 << 11,
    DPSession_ServerPlayerOnly    = 1 << 12,
    DPSession_ReliableProtocol    = 1 << 13,
    DPSession_NoOrder             = 1 << 14,
    DPSession_OptimiseLatency     = 1 << 15,
    DPSession_AcquireVoice        = 1 << 16,
    DPSession_NoSessionDescChange = 1 << 17,
};

struct DPSessionDesc2
{
    uint32_t size;
    uint32_t flags;
    uint8_t instanceGUID[16];
    uint8_t applicationGUID[16];
    uint32_t maxPlayers;
    uint32_t currentPlayerCount;

    // placeholders for 32-bit pointers
    uint32_t sessionName;
    uint32_t password;

    uint32_t reserved1; // xor-ed with player ids
    uint32_t reserved2;

    uint32_t applicationDefined1;
    uint32_t applicationDefined2;
    uint32_t applicationDefined3;
    uint32_t applicationDefined4;
};
static_assert(sizeof(DPSessionDesc2) == 80);

enum class DPSPCommand : uint16_t
{
    EnumSessionsReply       = 1,
    EnumSessions            = 2,
    EnumPlayersReply        = 3,
    EnumPlayer              = 4,
    RequestPlayerId         = 5,
    RequestGroupId          = 6,
    RequestPlayerReply      = 7,
    CreatePlayer            = 8,
    CreateGroup             = 9,
    PlayerMessage           = 10,
    DeletePlayer            = 11,
    DeleteGroup             = 12,
    AddPlayerToGroup        = 13,
    DeletePlayerFromGroup   = 14,
    PlayerDataChanged       = 15,
    PlayerNameChanged       = 16,
    GroupDataChanged        = 17,
    GroupNameChanged        = 18,
    AddForwardRequest       = 19,
    Packet                  = 21,
    Ping                    = 22,
    PingReply               = 23,
    YouAreDead              = 24,
    PlayerWrapper           = 25,
    SessionDescChanged      = 26,
    Challenge               = 28,
    AccessGranted           = 29,
    LogonDenied             = 30,
    AuthError               = 31,
    Negotiate               = 32,
    ChallengeResponse       = 33,
    Signed                  = 34,
    AddForwardReply         = 36,
    Ask4Multicast           = 37,
    Ask4MulticastGuaranteed = 38,
    AddShortcutToGroup      = 39,
    DeleteShortcutFromGroup = 40,
    SuperEnumPlayersReply   = 41,
    // ...
};

struct DPSPMessageHeader
{
    // these two are optional
    uint32_t sizeToken; // size | token << 20
    DPSockaddrIn sockaddr;

    char signature[4]; // "play"
    DPSPCommand command;
    uint16_t version; // 14 == dx9 (the last version)
};
static_assert(sizeof(DPSPMessageHeader) == 28);

// commands

struct DPSPMessageEnumSessionsReply
{
    // header

    DPSessionDesc2 sessionDescription;

    uint32_t nameOffset;
};
static_assert(sizeof(DPSPMessageEnumSessionsReply) == 84);

enum DPEnumSessionsFlags
{
    EnumSessions_Joinable         = 1 << 0,
    EnumSessions_All              = 1 << 1,
    EnumSessions_PasswordRequired = 1 << 6,
};

struct DPSPMessageEnumSessions
{
    // header

    uint8_t applicationGUID[16];
    uint32_t passwordOffset;
    uint32_t flags;
};
static_assert(sizeof(DPSPMessageEnumSessions) == 24);

// ...

enum DPRequestPlayerIdFlags
{
    RequestPlayerId_System         = 1 << 0,
    RequestPlayerId_SendingMachine = 1 << 3, // ignored
};

struct DPSPMessageRequestPlayerId
{
    // header

    uint32_t flags;
};
static_assert(sizeof(DPSPMessageRequestPlayerId) == 4);

// requestgroupid ...

struct DPSPMessageRequestPlayerReply
{
    // header

    uint32_t id;

    DPSecurityDesc securityDesc;

    uint32_t sspiProviderOffset;
    uint32_t capiProviderOffset;
    uint32_t result;
};
static_assert(sizeof(DPSPMessageRequestPlayerReply) == 40);

// this is just AddForwardRequest with ignored fields...
struct DPSPMessageCreatePlayer
{
    // header

    uint32_t idTo; // ignored/zero
    uint32_t playerId;
    uint32_t groupId; // ignored/zero
    uint32_t createOffset; // must be 28
    uint32_t passwordOffset; // ignored/zero

    // player info
};
static_assert(sizeof(DPSPMessageCreatePlayer) == 20);

// ...

struct DPSPMessageAddForwardRequest
{
    // header

    uint32_t idTo;
    uint32_t playerId;
    uint32_t groupId;
    uint32_t createOffset; // should be 28
    uint32_t passwordOffset;

    // player info, password, tick count
};
static_assert(sizeof(DPSPMessageAddForwardRequest) == 20);

struct DPSPMessagePacket
{
    // header

    uint8_t messageGUID[16];

    uint32_t packetIndex;
    uint32_t dataSize;
    uint32_t offset;
    uint32_t totalPackets;
    uint32_t messageSize;

    uint32_t packedOffset;
};
static_assert(sizeof(DPSPMessagePacket) == 40);

// ...

struct DPSPMessageSuperEnumPlayersReply
{
    // header

    uint32_t playerCount;
    uint32_t groupCount;
    uint32_t packedOffset;
    uint32_t shortcutCount;
    uint32_t descriptionOffset;
    uint32_t nameOffset;
    uint32_t passwordOffset;

    // session desc
    // session name
    // password
    // superpackedplayer
};
static_assert(sizeof(DPSPMessageSuperEnumPlayersReply) == 28);


// "reliable protocol" bits

enum DPRPFrameFlags
{
    DPRPFrame_Reliable = 1 << 0, // because being the RELIABLE protocol isn't enough?
    DPRPFrame_Ack      = 1 << 1,
    DPRPFrame_SendAck  = 1 << 2,
    DPRPFrame_End      = 1 << 3,
    DPRPFrame_Start    = 1 << 4,
    DPRPFrame_Command  = 1 << 5,
    DPRPFrame_Big      = 1 << 6, // unimplemented
    DPRPFrame_Extended = 1 << 7, // also unimplemented?
};