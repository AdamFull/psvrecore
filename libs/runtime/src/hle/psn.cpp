// PlayStation Network beyond its start (np.cpp, online.cpp), as on a console that is offline and signed out: every
// function the surveyed games import from the NP libraries. Libraries start and end, contexts and requests that need
// nothing from PSN are made (each a new positive ID) and given back, callbacks and handlers are taken and never
// called; whatever needs PSN answers its library's own "signed out" error (or, where the library has none, the error
// it gives when what is asked about does not exist: no signaling context, not in a party, no gift found nearby).
// Functions known only by NID (imported by a game's own NP modules) answer the same.
//
// Done here for real: NP IDs compared (sceNpCmpNpId, sceNpCmpNpIdInOrder), the NP environment's name, the party state
// (not in a party), an NP ID read from text (sceNpWebApiUtilityParseNpId). Trophies are kept by np.cpp; their
// configuration and icons (in the game's encrypted trophy file) are not read, so the information functions answer
// that the trophy file is unsupported, and the icon functions that there is no icon.

#include <atomic>
#include <cstring>

#include "hle_common.hpp"

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceNpManager = 0xD8835093, kSceNpCommon = 0xECB62224, kSceNpBasic = 0x605BA760,
              kSceNpScore = 0x75F052DB, kSceNpTus = 0xAE84BDEA, kSceNpUtility = 0x7B47D9CF, kSceNpTrophy = 0x4332C10D,
              kSceNpActivity = 0xFEF492ED, kSceNpMessage = 0x8134120B, kSceNpSnsFacebook = 0x1EF501B8,
              kSceNpSignaling = 0xEE59FC02, kSceNpPartyGameUtil = 0xB3B34742, kSceNpCommerce2 = 0x40120DC3,
              kSceNpMatching2 = 0x7106F303, kSceNpWebApi = 0xE1AF627C, kSceNearUtil = 0x560D1608;

// Each library's answer for what needs PSN.
constexpr i32 kManagerSignedOut = err::code(0x8055050A), kAuthSignedOut = err::code(0x80550310),
              kBasicSignedOut = err::code(0x80551D06), kMessageNotSignedIn = err::code(0x80551E0C),
              kFacebookNotSignedIn = err::code(0x80552607), kSignalingCtxNotFound = err::code(0x80552705),
              kPartyNotInParty = err::code(0x80552506), kCommerce2NotSignedIn = err::code(0x80550F72),
              kMatching2SignedOut = err::code(0x80550C2B), kWebApiNotSignedIn = err::code(0x80552907),
              kNearNotRetrieved = err::code(0x8010491E), kActivitySignedOut = err::code(0x80552306);
constexpr i32 kUtilInvalidArgument = err::code(0x80550601), kUtilNotMatch = err::code(0x80550609),
              kTrophyInvalidArgument = err::code(0x80551604), kTrophyIconNotFound = err::code(0x80551618),
              kTrophyUnsupportedTrp = err::code(0x8055161B), kPartyInvalidArgument = err::code(0x80552504),
              kWebApiInvalidArgument = err::code(0x80552902);

std::atomic<i32> g_next_id{0x100};
i32 done() { return 0; }
i32 new_id() { return g_next_id.fetch_add(1); }
template <i32 E>
i32 fail() {
    return E;
}

#define DONE(nid, name) Function{nid, name, &entry<&done>}
#define NEW_ID(nid, name) Function{nid, name, &entry<&new_id>}
#define FAIL(nid, name, code) Function{nid, name, &entry<&fail<code>>}

// SceNpId: the online ID (16 characters, a terminator, padding), then options and reserved bytes (36 in all). Two are
// the same user when their online IDs are.
struct NpId {
    char data[16];
    char term;
    char dummy[3];
    u8 opt[8], reserved[8];
};
static_assert(sizeof(NpId) == 36);
i32 compare_ids(Ptr<const NpId> a, Ptr<const NpId> b, i32* order) {
    if (!a || !b || !readable(a.address, sizeof(NpId)) || !readable(b.address, sizeof(NpId))) return kUtilInvalidArgument;
    const i32 c = std::strncmp(a->data, b->data, 16);
    if (order) *order = c < 0 ? -1 : c > 0 ? 1 : 0;
    return c == 0 ? 0 : kUtilNotMatch;
}
i32 sceNpCmpNpId(Ptr<const NpId> a, Ptr<const NpId> b) { return compare_ids(a, b, nullptr); }
i32 sceNpCmpNpIdInOrder(Ptr<const NpId> a, Ptr<const NpId> b, Ptr<i32> order) {
    if (!order || !writable(order.address, 4)) return kUtilInvalidArgument;
    i32 o = 0;
    const i32 r = compare_ids(a, b, &o);
    if (r < 0 && r != kUtilNotMatch) return r;
    *order = o;
    return 0;
}
// The NP environment a retail console is in: "np".
i32 sceNpCommonGetNpEnviroment(Ptr<char> out) {
    if (!out || !writable(out.address, 3)) return kUtilInvalidArgument;
    std::memcpy(out.get(), "np", 3);
    return 0;
}
// SCE_NP_PARTY_STATE_NOT_IN_PARTY
i32 sceNpPartyGetState(Ptr<i32> state) {
    if (!state || !writable(state.address, 4)) return kPartyInvalidArgument;
    *state = 2;
    return 0;
}
// An online ID in text (as PSN's web APIs give them) into an NP ID.
i32 sceNpWebApiUtilityParseNpId(Ptr<const char> text, Ptr<NpId> out) {
    if (!text || !out || !writable(out.address, sizeof(NpId))) return kWebApiInvalidArgument;
    const nx::string id = str(text, 64);
    if (id.empty() || id.size() > 16) return kWebApiInvalidArgument;
    std::memset(out.get(), 0, sizeof(NpId));
    std::memcpy(out->data, id.data(), id.size());
    return 0;
}
i32 sceNpTrophyAbortHandle(i32 handle) { return handle > 0 ? 0 : kTrophyInvalidArgument; }

const Function kNpManagerMore[] = {
    FAIL(0x29C9B8A5, "(SceNpManager 0x29C9B8A5)", kManagerSignedOut),
    FAIL(0x7CE94530, "(SceNpManager 0x7CE94530)", kManagerSignedOut),
    FAIL(0x80036A44, "(SceNpManager 0x80036A44)", kManagerSignedOut),
    FAIL(0x8D7981D4, "(SceNpManager 0x8D7981D4)", kManagerSignedOut),
    FAIL(0x9554802B, "(SceNpManager 0x9554802B)", kManagerSignedOut),
    FAIL(0x96DF1B76, "(SceNpManager 0x96DF1B76)", kManagerSignedOut),
    FAIL(0x555EB58C, "sceNpAuthCreateOAuthRequest", kManagerSignedOut),
    DONE(0x8C94C7A5, "sceNpAuthDeleteOAuthRequest"),
    FAIL(0x65DFB208, "sceNpAuthGetAuthorizationCode", kManagerSignedOut),
    FAIL(0xFE835967, "sceNpManagerGetAccountRegion", kManagerSignedOut),
    FAIL(0x43DC48A1, "sceNpManagerGetCachedParam", kManagerSignedOut),
    DONE(0xD9E6E56C, "sceNpUnregisterServiceStateCallback"),
};
const Function kNpCommonMore[] = {
    FAIL(0x40724BEA, "(SceNpCommon 0x40724BEA)", kAuthSignedOut),
    FAIL(0x48314C6E, "(SceNpCommon 0x48314C6E)", kAuthSignedOut),
    FAIL(0x650D111B, "(SceNpCommon 0x650D111B)", kAuthSignedOut),
    FAIL(0x88B18A5B, "(SceNpCommon 0x88B18A5B)", kAuthSignedOut),
    FAIL(0x9AEF645B, "(SceNpCommon 0x9AEF645B)", kAuthSignedOut),
    FAIL(0xBDCD318E, "(SceNpCommon 0xBDCD318E)", kAuthSignedOut),
    FAIL(0xE426FEE8, "(SceNpCommon 0xE426FEE8)", kAuthSignedOut),
    DONE(0x14FC18AF, "sceNpAuthDestroyRequest"),
    FAIL(0xF93842F0, "sceNpAuthGetEntitlementById", kAuthSignedOut),
    FAIL(0x3377CD37, "sceNpAuthGetEntitlementIdList", kAuthSignedOut),
    FAIL(0x59608D1C, "sceNpAuthGetTicket", kAuthSignedOut),
    FAIL(0xC1E23E01, "sceNpAuthGetTicketParam", kAuthSignedOut),
    DONE(0xE807D0BC, "sceNpSetPlatformType"),
};
const Function kNpBasicMore[] = {
    FAIL(0x1211AE8E, "sceNpBasicGetBlockListEntries", kBasicSignedOut),
    FAIL(0x407E1E6F, "sceNpBasicGetBlockListEntryCount", kBasicSignedOut),
    FAIL(0x5183A4B5, "sceNpBasicGetFriendOnlineStatus", kBasicSignedOut),
    FAIL(0xEF8A91BC, "sceNpBasicGetGamePresenceOfFriend", kBasicSignedOut),
    FAIL(0x364531A8, "sceNpBasicGetPlaySessionLog", kBasicSignedOut),
    FAIL(0xFB0F7FDF, "sceNpBasicGetPlaySessionLogSize", kBasicSignedOut),
    FAIL(0x8C90CC09, "sceNpBasicJoinGameAckResponseSend", kBasicSignedOut),
    FAIL(0x3B0A7F47, "sceNpBasicRecordPlaySessionLog", kBasicSignedOut),
    DONE(0x3ADE3628, "sceNpBasicRegisterInGameDataMessageHandler"),
    DONE(0xE02A445C, "sceNpBasicRegisterJoinGameAckHandler"),
    FAIL(0x7A5020A5, "sceNpBasicSendInGameDataMessage", kBasicSignedOut),
    DONE(0x363FF161, "sceNpBasicUnregisterInGameDataMessageHandler"),
    DONE(0x2A764628, "sceNpBasicUnregisterJoinGameAckHandler"),
};
const Function kNpScoreMore[] = {
    FAIL(0x00F90E7B, "sceNpScoreGetBoardInfo", kManagerSignedOut),
    FAIL(0x4F795659, "sceNpScoreGetFriendsRanking", kManagerSignedOut),
    FAIL(0x7090ED11, "sceNpScoreGetFriendsRankingAsync", kManagerSignedOut),
    FAIL(0xDFAD64D3, "sceNpScoreGetGameData", kManagerSignedOut),
    FAIL(0xCE416993, "sceNpScoreGetGameDataAsync", kManagerSignedOut),
    FAIL(0xBAE55B34, "sceNpScoreGetRankingByNpId", kManagerSignedOut),
    FAIL(0x45CD1D00, "sceNpScoreGetRankingByNpIdAsync", kManagerSignedOut),
    FAIL(0xC45E3FCD, "sceNpScoreGetRankingByRangeAsync", kManagerSignedOut),
    FAIL(0xC2862B67, "sceNpScoreRecordGameData", kManagerSignedOut),
    FAIL(0x40573917, "sceNpScoreRecordGameDataAsync", kManagerSignedOut),
    FAIL(0x320C0277, "sceNpScoreRecordScore", kManagerSignedOut),
    DONE(0x5EF44841, "sceNpScoreSetTimeout"),
};
const Function kNpTusMore[] = {
    FAIL(0x8CA469F4, "sceNpTssGetSmallStorage", kManagerSignedOut),
    FAIL(0xEA588F23, "sceNpTssGetSmallStorageAsync", kManagerSignedOut),
    FAIL(0xBB5E5E29, "sceNpTusAddAndGetVariableVUserAsync", kManagerSignedOut),
    FAIL(0x0CFAC523, "sceNpTusDeleteMultiSlotDataAsync", kManagerSignedOut),
    FAIL(0x883AC44A, "sceNpTusGetDataAsync", kManagerSignedOut),
    FAIL(0x520975C3, "sceNpTusGetMultiSlotDataStatusAsync", kManagerSignedOut),
    FAIL(0xD26B9F4C, "sceNpTusGetMultiSlotVariableAsync", kManagerSignedOut),
    FAIL(0xF91887EC, "sceNpTusGetMultiSlotVariableVUserAsync", kManagerSignedOut),
    FAIL(0xC003C588, "sceNpTusPollAsync", kManagerSignedOut),
    FAIL(0x9FABA1E5, "sceNpTusSetDataAsync", kManagerSignedOut),
    FAIL(0xA61AEDB0, "sceNpTusSetMultiSlotVariableAsync", kManagerSignedOut),
    FAIL(0x2E9A7FD0, "sceNpTusSetMultiSlotVariableVUserAsync", kManagerSignedOut),
    DONE(0xC4BB64A7, "sceNpTusSetTimeout"),
    FAIL(0x44947D07, "sceNpTusTryAndSetVariableAsync", kManagerSignedOut),
    FAIL(0x518A64B9, "sceNpTusTryAndSetVariableVUserAsync", kManagerSignedOut),
};
const Function kNpUtilityMore[] = {
    DONE(0x32B068C4, "sceNpBandwidthTestAbort"),
    FAIL(0xE0EBFBF6, "sceNpBandwidthTestGetStatus", kManagerSignedOut),
    DONE(0x081FA13C, "sceNpBandwidthTestInitStart"),
    DONE(0x58D92EFD, "sceNpBandwidthTestShutdown"),
    DONE(0x027587C4, "sceNpLookupAbortRequest"),
    FAIL(0x6A1BF429, "sceNpLookupUserProfile", kManagerSignedOut),
    FAIL(0xE5285E0F, "sceNpLookupUserProfileAsync", kManagerSignedOut),
    FAIL(0xCF956F23, "sceNpLookupWaitAsync", kManagerSignedOut),
    DONE(0x36DA662E, "sceNpWordFilterAbortRequest"),
    FAIL(0xA30E4E17, "sceNpWordFilterCensorComment", kManagerSignedOut),
    FAIL(0xE956080F, "sceNpWordFilterCensorCommentAsync", kManagerSignedOut),
    NEW_ID(0x7183047C, "sceNpWordFilterCreateRequest"),
    NEW_ID(0x1075589C, "sceNpWordFilterCreateTitleCtx"),
    DONE(0x6690D8E2, "sceNpWordFilterDeleteRequest"),
    DONE(0xB4E5A189, "sceNpWordFilterDeleteTitleCtx"),
    DONE(0x01284C6E, "sceNpWordFilterInit"),
    FAIL(0xBAE4CD1F, "sceNpWordFilterPollAsync", kManagerSignedOut),
    FAIL(0x30606C85, "sceNpWordFilterSanitizeComment", kManagerSignedOut),
    FAIL(0x40EC7271, "sceNpWordFilterSanitizeCommentAsync", kManagerSignedOut),
    DONE(0x9F44CE45, "sceNpWordFilterSetTimeout"),
    DONE(0x7D140F70, "sceNpWordFilterTerm"),
};
const Function kNpMessageMore[] = {
    FAIL(0x7E697F98, "sceNpMessageGetAttachedData", kMessageNotSignedIn),
    FAIL(0xC86B6DA7, "sceNpMessageGetMessageEntries", kMessageNotSignedIn),
    FAIL(0x1DBD26D2, "sceNpMessageGetMessageEntryCount", kMessageNotSignedIn),
    DONE(0x258DA4AC, "sceNpMessageInit"),
    DONE(0xF533A73A, "sceNpMessageSetAttachedDataUsedFlag"),
    FAIL(0x35BE21C5, "sceNpMessageSyncMessage", kMessageNotSignedIn),
    DONE(0x380230A1, "sceNpMessageTerm"),
};
const Function kNpSnsFacebookMore[] = {
    FAIL(0x9CF1BC76, "sceNpSnsFacebookCreateRequest", kFacebookNotSignedIn),
    DONE(0xA537FF90, "sceNpSnsFacebookDeleteRequest"),
    FAIL(0xA3C9041D, "sceNpSnsFacebookGetLongAccessToken", kFacebookNotSignedIn),
    DONE(0x80557AA0, "sceNpSnsFacebookInit"),
    DONE(0xA71E6847, "sceNpSnsFacebookTerm"),
};
const Function kNpSignalingMore[] = {
    FAIL(0x92FFBDE3, "sceNpSignalingActivateConnection", kSignalingCtxNotFound),
    FAIL(0xF77EF683, "sceNpSignalingCreateCtx", kSignalingCtxNotFound),
    DONE(0x3B56E6C2, "sceNpSignalingDeactivateConnection"),
    DONE(0xEAA4B1F3, "sceNpSignalingDestroyCtx"),
    FAIL(0x51883EAE, "sceNpSignalingGetConnectionInfo", kSignalingCtxNotFound),
    FAIL(0x83B3DA0A, "sceNpSignalingGetConnectionStatus", kSignalingCtxNotFound),
    DONE(0x4B6ACF47, "sceNpSignalingInit"),
    DONE(0xBC892D18, "sceNpSignalingTerm"),
};
const Function kNpPartyGameUtilMore[] = {
    DONE(0xB6132502, "sceNpPartyCheckCallback"),
    FAIL(0x0560D9A1, "sceNpPartyGetMemberInfo", kPartyNotInParty),
    FAIL(0xFB200A6D, "sceNpPartyGetMemberVoiceInfo", kPartyNotInParty),
    FAIL(0x420C30E9, "sceNpPartyGetMembers", kPartyNotInParty),
    DONE(0xB304962C, "sceNpPartyInit"),
    DONE(0x324C76F0, "sceNpPartyRegisterHandler"),
    DONE(0xF524AD2B, "sceNpPartyTerm"),
};
const Function kNpCommerce2More[] = {
    DONE(0xE7486A60, "sceNpCommerce2AbortReq"),
    FAIL(0x123E55F4, "sceNpCommerce2CreateCtx", kCommerce2NotSignedIn),
    FAIL(0xFDB39774, "sceNpCommerce2CreateSessionCreateReq", kCommerce2NotSignedIn),
    FAIL(0xAEE8D3DF, "sceNpCommerce2CreateSessionGetResult", kCommerce2NotSignedIn),
    FAIL(0xBBDDF866, "sceNpCommerce2CreateSessionStart", kCommerce2NotSignedIn),
    DONE(0x6CD27BD0, "sceNpCommerce2DestroyCtx"),
    DONE(0x9CD463F8, "sceNpCommerce2DestroyGetCategoryContentsResult"),
    DONE(0x24D3FE34, "sceNpCommerce2DestroyGetProductInfoListResult"),
    DONE(0xADF3BD5B, "sceNpCommerce2DestroyGetProductInfoResult"),
    DONE(0xA4464754, "sceNpCommerce2DestroyReq"),
    FAIL(0x5CF665E0, "sceNpCommerce2GetCategoryContentsCreateReq", kCommerce2NotSignedIn),
    FAIL(0xC47CDB59, "sceNpCommerce2GetCategoryContentsGetResult", kCommerce2NotSignedIn),
    FAIL(0x774F88D0, "sceNpCommerce2GetCategoryContentsStart", kCommerce2NotSignedIn),
    FAIL(0xB88DB7C2, "sceNpCommerce2GetCategoryInfo", kCommerce2NotSignedIn),
    FAIL(0x56B80FAF, "sceNpCommerce2GetCategoryInfoFromContentInfo", kCommerce2NotSignedIn),
    FAIL(0xDE45F84F, "sceNpCommerce2GetContentInfo", kCommerce2NotSignedIn),
    FAIL(0x315B9BD0, "sceNpCommerce2GetContentRatingDescriptor", kCommerce2NotSignedIn),
    FAIL(0x7894DCDF, "sceNpCommerce2GetContentRatingInfoFromGameProductInfo", kCommerce2NotSignedIn),
    FAIL(0xA308B496, "sceNpCommerce2GetGameProductInfo", kCommerce2NotSignedIn),
    FAIL(0x021DC797, "sceNpCommerce2GetGameProductInfoFromContentInfo", kCommerce2NotSignedIn),
    FAIL(0x0550C016, "sceNpCommerce2GetGameProductInfoFromGetProductInfoListResult", kCommerce2NotSignedIn),
    FAIL(0x3B7C81A5, "sceNpCommerce2GetGameSkuInfoFromGameProductInfo", kCommerce2NotSignedIn),
    FAIL(0x88D236DF, "sceNpCommerce2GetPrice", kCommerce2NotSignedIn),
    FAIL(0xB0AFF6C9, "sceNpCommerce2GetProductInfoCreateReq", kCommerce2NotSignedIn),
    FAIL(0x363922BA, "sceNpCommerce2GetProductInfoGetResult", kCommerce2NotSignedIn),
    FAIL(0xE81B8BAD, "sceNpCommerce2GetProductInfoListCreateReq", kCommerce2NotSignedIn),
    FAIL(0x4DFBCDD6, "sceNpCommerce2GetProductInfoListGetResult", kCommerce2NotSignedIn),
    FAIL(0x18BDC4BD, "sceNpCommerce2GetProductInfoListStart", kCommerce2NotSignedIn),
    FAIL(0xC594ADC2, "sceNpCommerce2GetProductInfoStart", kCommerce2NotSignedIn),
    FAIL(0xF6F23623, "sceNpCommerce2GetSessionInfo", kCommerce2NotSignedIn),
    DONE(0xFFC8B18B, "sceNpCommerce2HidePsStoreIcon"),
    DONE(0xC73F209A, "sceNpCommerce2Init"),
    DONE(0x5135EA31, "sceNpCommerce2InitGetCategoryContentsResult"),
    DONE(0x29186E27, "sceNpCommerce2InitGetProductInfoListResult"),
    DONE(0x118E1C5E, "sceNpCommerce2InitGetProductInfoResult"),
    DONE(0x84E56A6D, "sceNpCommerce2ShowPsStoreIcon"),
    FAIL(0x7132EAA5, "sceNpCommerce2StartEmptyStoreCheck", kCommerce2NotSignedIn),
    DONE(0xB99958AE, "sceNpCommerce2Term"),
};
const Function kNpMatching2More[] = {
    FAIL(0xBB2E7559, "sceNpMatching2ContextStart", kMatching2SignedOut),
    DONE(0x506454DE, "sceNpMatching2ContextStop"),
    FAIL(0xADF578E1, "sceNpMatching2CreateContext", kMatching2SignedOut),
    FAIL(0x818A9499, "sceNpMatching2CreateJoinRoom", kMatching2SignedOut),
    DONE(0x368AA759, "sceNpMatching2DestroyContext"),
    FAIL(0x5BE8B86F, "sceNpMatching2GetRoomDataExternalList", kMatching2SignedOut),
    FAIL(0xC61C79EE, "sceNpMatching2GetRoomDataInternal", kMatching2SignedOut),
    FAIL(0x28C643B7, "sceNpMatching2GetRoomMemberDataInternal", kMatching2SignedOut),
    FAIL(0x65C0FEED, "sceNpMatching2GetServerLocal", kMatching2SignedOut),
    FAIL(0x78521326, "sceNpMatching2GetUserInfoList", kMatching2SignedOut),
    FAIL(0xC086B560, "sceNpMatching2GetWorldInfoList", kMatching2SignedOut),
    DONE(0xEBB1FE74, "sceNpMatching2Init"),
    FAIL(0x33F7D5AE, "sceNpMatching2JoinRoom", kMatching2SignedOut),
    FAIL(0xA8021394, "sceNpMatching2KickoutRoomMember", kMatching2SignedOut),
    FAIL(0xC8B0C9EE, "sceNpMatching2LeaveRoom", kMatching2SignedOut),
    DONE(0xF9E35566, "sceNpMatching2RegisterContextCallback"),
    DONE(0xF486991B, "sceNpMatching2RegisterRoomEventCallback"),
    DONE(0xFA51949B, "sceNpMatching2RegisterRoomMessageCallback"),
    DONE(0x1C60BC5B, "sceNpMatching2RegisterSignalingCallback"),
    FAIL(0xD48BAF13, "sceNpMatching2SearchRoom", kMatching2SignedOut),
    FAIL(0xE0BE0510, "sceNpMatching2SendRoomChatMessage", kMatching2SignedOut),
    FAIL(0x7B908D99, "sceNpMatching2SendRoomMessage", kMatching2SignedOut),
    DONE(0xF3A43C50, "sceNpMatching2SetDefaultRequestOptParam"),
    FAIL(0x8F88AC7E, "sceNpMatching2SetRoomDataExternal", kMatching2SignedOut),
    FAIL(0x94784A8C, "sceNpMatching2SetRoomDataInternal", kMatching2SignedOut),
    FAIL(0xD3096C55, "sceNpMatching2SetRoomMemberDataInternal", kMatching2SignedOut),
    FAIL(0xF049BEC1, "sceNpMatching2SetUserInfo", kMatching2SignedOut),
    FAIL(0x20598618, "sceNpMatching2SignalingGetConnectionInfo", kMatching2SignedOut),
    FAIL(0x4E4C55BD, "sceNpMatching2SignalingGetConnectionStatus", kMatching2SignedOut),
    FAIL(0x79310806, "sceNpMatching2SignalingGetLocalNetInfo", kMatching2SignedOut),
    DONE(0x0124641C, "sceNpMatching2Term"),
};
const Function kNpWebApiMore[] = {
    DONE(0xEB3D14CE, "sceNpWebApiAbortRequest"),
    DONE(0xEDA9C359, "sceNpWebApiCheckCallback"),
    NEW_ID(0x40341759, "sceNpWebApiCreateHandle"),
    NEW_ID(0x18CB54B5, "sceNpWebApiCreatePushEventFilter"),
    FAIL(0x6FC7357E, "sceNpWebApiCreateRequest", kWebApiNotSignedIn),
    NEW_ID(0x2B61B0C7, "sceNpWebApiCreateServicePushEventFilter"),
    DONE(0x6099B918, "sceNpWebApiDeletePushEventFilter"),
    DONE(0x57B2826E, "sceNpWebApiDeleteRequest"),
    FAIL(0xB0500FF4, "sceNpWebApiGetHttpResponseHeaderValue", kWebApiNotSignedIn),
    FAIL(0xD585FDEA, "sceNpWebApiGetHttpResponseHeaderValueLength", kWebApiNotSignedIn),
    NEW_ID(0x8519EB34, "sceNpWebApiInitialize"),
    FAIL(0xBA75BF6E, "sceNpWebApiReadData", kWebApiNotSignedIn),
    NEW_ID(0xDB04ECD6, "sceNpWebApiRegisterPushEventCallback"),
    NEW_ID(0xF6B6C994, "sceNpWebApiRegisterServicePushEventCallback"),
    FAIL(0x1E37862A, "sceNpWebApiSendRequest2", kWebApiNotSignedIn),
    DONE(0x001254C2, "sceNpWebApiTerminate"),
    DONE(0xC9056736, "sceNpWebApiUnregisterPushEventCallback"),
    DONE(0x06D28ADA, "sceNpWebApiUnregisterServicePushEventCallback"),
};
const Function kNearUtilMore[] = {
    FAIL(0x1AA394BA, "(SceNearUtil 0x1AA394BA)", kNearNotRetrieved),
    FAIL(0x1AE3EC1C, "(SceNearUtil 0x1AE3EC1C)", kNearNotRetrieved),
    FAIL(0x49A97D5F, "(SceNearUtil 0x49A97D5F)", kNearNotRetrieved),
    FAIL(0x69EE6FB3, "(SceNearUtil 0x69EE6FB3)", kNearNotRetrieved),
    FAIL(0x8095FDEF, "(SceNearUtil 0x8095FDEF)", kNearNotRetrieved),
    FAIL(0x88540CEC, "(SceNearUtil 0x88540CEC)", kNearNotRetrieved),
    FAIL(0x9B3F2DCE, "(SceNearUtil 0x9B3F2DCE)", kNearNotRetrieved),
    FAIL(0x9FB9277B, "(SceNearUtil 0x9FB9277B)", kNearNotRetrieved),
    FAIL(0xA412E9CA, "(SceNearUtil 0xA412E9CA)", kNearNotRetrieved),
    FAIL(0xBF2BBE1F, "(SceNearUtil 0xBF2BBE1F)", kNearNotRetrieved),
    FAIL(0xD22CB2DE, "(SceNearUtil 0xD22CB2DE)", kNearNotRetrieved),
    FAIL(0xDDEB6C8C, "(SceNearUtil 0xDDEB6C8C)", kNearNotRetrieved),
    FAIL(0xE608000B, "(SceNearUtil 0xE608000B)", kNearNotRetrieved),
    DONE(0x88A68D20, "sceNearCloseDiscoveredGiftImage"),
    DONE(0x2F35C196, "sceNearCloseReceivedGiftData"),
    FAIL(0x80D6C552, "sceNearConvertDiscoveredGiftParam", kNearNotRetrieved),
    FAIL(0x8CBEB2DA, "sceNearDeleteDiscoveredGift", kNearNotRetrieved),
    DONE(0x52021026, "sceNearDeleteGift"),
    DONE(0xFF3BC581, "sceNearFinalize"),
    DONE(0x3F3F6D92, "sceNearFinalizeAndLaunchNearApp"),
    FAIL(0x773ABEA9, "sceNearGetDiscoveredGiftInfo", kNearNotRetrieved),
    FAIL(0xEFE6F060, "sceNearGetDiscoveredGiftSender", kNearNotRetrieved),
    FAIL(0xA64D9D38, "sceNearGetDiscoveredGiftStatus", kNearNotRetrieved),
    FAIL(0xDE6F0859, "sceNearGetDiscoveredGifts", kNearNotRetrieved),
    FAIL(0xDB8BAC35, "sceNearGetGift", kNearNotRetrieved),
    FAIL(0xD393578E, "sceNearGetLastNeighborFoundDateTime", kNearNotRetrieved),
    FAIL(0xAEF3022A, "sceNearGetMyStatus", kNearNotRetrieved),
    FAIL(0xAD264F5F, "sceNearGetNeighbors", kNearNotRetrieved),
    FAIL(0x6C2E610F, "sceNearGetNewNeighbors", kNearNotRetrieved),
    FAIL(0xE75A5A9B, "sceNearGetRecentNeighbors", kNearNotRetrieved),
    DONE(0xBBCA5813, "sceNearInitialize"),
    FAIL(0xB22CB01D, "sceNearLaunchNearAppForDownload", kNearNotRetrieved),
    FAIL(0xF3398774, "sceNearLaunchNearAppForUpdate", kNearNotRetrieved),
    FAIL(0xF8C15008, "sceNearOpenDiscoveredGiftImage", kNearNotRetrieved),
    FAIL(0x06807CA3, "sceNearOpenReceivedGiftData", kNearNotRetrieved),
    FAIL(0x2364D6BD, "sceNearReadDiscoveredGiftImage", kNearNotRetrieved),
    FAIL(0xB14145DD, "sceNearReadReceivedGiftData", kNearNotRetrieved),
    DONE(0x2ED36EE2, "sceNearRefresh"),
    DONE(0x146BE236, "sceNearSetGift"),
};

const Function kNpCommonManual[] = {
    HLE_FUNCTION(0xFB8D82E5, sceNpCmpNpId),
    HLE_FUNCTION(0x6BC8150A, sceNpCmpNpIdInOrder),
    HLE_FUNCTION(0xAD6403E0, sceNpCommonGetNpEnviroment),
};
const Function kNpPartyManual[] = {HLE_FUNCTION(0x9F99ADF7, sceNpPartyGetState)};
const Function kNpWebApiManual[] = {HLE_FUNCTION(0x98CBE11F, sceNpWebApiUtilityParseNpId)};
const Function kNpTrophyMore[] = {
    HLE_FUNCTION(0xD55C6F4C, sceNpTrophyAbortHandle),
    FAIL(0xBA2B7F2A, "sceNpTrophyGetGameInfo", kTrophyUnsupportedTrp),
    FAIL(0x087B0535, "sceNpTrophyGetGroupInfo", kTrophyUnsupportedTrp),
    FAIL(0xA4ADDD91, "sceNpTrophyGetTrophyInfo", kTrophyUnsupportedTrp),
    FAIL(0xFE382529, "sceNpTrophyGetGameIcon", kTrophyIconNotFound),
    FAIL(0x1B8C3192, "sceNpTrophyGetGroupIcon", kTrophyIconNotFound),
    FAIL(0x94BAB8D0, "sceNpTrophyGetTrophyIcon", kTrophyIconNotFound),
};
const Function kNpActivityMore[] = {FAIL(0x6BE90416, "sceNpActivityPostAppStartupStatus", kActivitySignedOut)};

}  // namespace

void register_psn() {
    register_library({"SceNpManager", kSceNpManager, kNpManagerMore});
    register_library({"SceNpCommon", kSceNpCommon, kNpCommonMore});
    register_library({"SceNpCommon", kSceNpCommon, kNpCommonManual});
    register_library({"SceNpBasic", kSceNpBasic, kNpBasicMore});
    register_library({"SceNpScore", kSceNpScore, kNpScoreMore});
    register_library({"SceNpTus", kSceNpTus, kNpTusMore});
    register_library({"SceNpUtility", kSceNpUtility, kNpUtilityMore});
    register_library({"SceNpTrophy", kSceNpTrophy, kNpTrophyMore});
    register_library({"SceNpActivity", kSceNpActivity, kNpActivityMore});
    register_library({"SceNpMessage", kSceNpMessage, kNpMessageMore});
    register_library({"SceNpSnsFacebook", kSceNpSnsFacebook, kNpSnsFacebookMore});
    register_library({"SceNpSignaling", kSceNpSignaling, kNpSignalingMore});
    register_library({"SceNpPartyGameUtil", kSceNpPartyGameUtil, kNpPartyGameUtilMore});
    register_library({"SceNpPartyGameUtil", kSceNpPartyGameUtil, kNpPartyManual});
    register_library({"SceNpCommerce2", kSceNpCommerce2, kNpCommerce2More});
    register_library({"SceNpMatching2", kSceNpMatching2, kNpMatching2More});
    register_library({"SceNpWebApi", kSceNpWebApi, kNpWebApiMore});
    register_library({"SceNpWebApi", kSceNpWebApi, kNpWebApiManual});
    register_library({"SceNearUtil", kSceNearUtil, kNearUtilMore});
}

}  // namespace psvrecore::runtime::hle
