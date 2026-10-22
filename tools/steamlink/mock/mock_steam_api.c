/*
 * Mock steam_api64.dll (libsteam_api.so on Linux) for testing OOTSteamLink with a host and a joiner on one machine, without Steam.
 * Derived from the HXCoop / KeeperFX mock (same fake lobby and message design), built as a 64 bit DLL.
 *
 * It exports the subset of the Steamworks flat C API that OOTSteamLink uses and fakes:
 *  - the Steam ID: from the STEAMLINK_MOCK_NAME environment variable ("Host" = 1, "Client" = 2, others hashed);
 *  - lobbies: lobby id = LOBBY_BASE + owner number; the owner tracks its members;
 *  - friends: "Host" is the only friend of everyone else and is reported as playing AppID 480 in its lobby;
 *  - ISteamNetworkingMessages: datagrams over localhost UDP, port base + number. Like the real thing, the first
 *    message from an unknown user raises SteamNetworkingMessagesSessionRequest_t and is held until
 *    AcceptSessionWithUser; there is no notification when a peer goes away.
 * Set STEAMLINK_MOCK_LOG=1 to write mock_steam.log in the working directory.
 * STEAMLINK_MOCK_PORT_BASE moves the UDP port range so that test runs can happen side by side.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#define API __declspec(dllexport)
#define NET_RESET() (WSAGetLastError() == WSAECONNRESET)
#else
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define API __attribute__((visibility("default")))
#define NET_RESET() (errno == ECONNREFUSED)
typedef int SOCKET;
typedef uint32_t DWORD;
typedef uint16_t u_short;
#define INVALID_SOCKET (-1)
#define _stricmp strcasecmp
static DWORD GetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
#endif
#define STEAMID_BASE 76561198000000000ULL
#define LOBBY_BASE 0x0186000000000000ULL

typedef uint64_t u64;

static int g_inited, g_log;
#ifdef _WIN32
static int g_wsa;
#endif
static unsigned g_num;
static unsigned g_port_base = 47000;
static u64 g_sid;
static SOCKET g_sock = INVALID_SOCKET;
static FILE *g_logf;

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    if (!g_logf) g_logf = fopen("mock_steam.log", "a");
    if (!g_logf) return;
    va_list a; va_start(a, fmt);
    vfprintf(g_logf, fmt, a);
    va_end(a);
    fputc(10, g_logf);
    fflush(g_logf);
}

/* ---- identity ---- */
static unsigned name_to_num(const char *name)
{
    if (_stricmp(name, "Host") == 0) return 1;
    if (_stricmp(name, "Client") == 0) return 2;
    uint32_t h = 2166136261u;
    for (; *name; name++) { h ^= (unsigned char)*name; h *= 16777619u; }
    return 10 + h % 900;
}

static void init_identity(void)
{
    if (g_num) return;
    char name[64] = "";
    const char *env = getenv("STEAMLINK_MOCK_NAME");
    if (env) snprintf(name, sizeof(name), "%s", env);
    else {
#ifdef _WIN32
        const char *cl = GetCommandLineA();
#else
        const char *cl = "";
#endif
        const char *p = strstr(cl, "-nick ");
        if (p) {
            p += 6;
            size_t n = 0;
            while (p[n] && p[n] != ' ' && n < sizeof(name) - 1) { name[n] = p[n]; n++; }
            name[n] = 0;
        }
    }
    g_log = getenv("STEAMLINK_MOCK_LOG") != NULL;
    env = getenv("STEAMLINK_MOCK_PORT_BASE");
    if (env && atoi(env) > 1024) g_port_base = (unsigned)atoi(env);
    g_num = name[0] ? name_to_num(name) : 3;
    g_sid = STEAMID_BASE + g_num;
    logf_("identity name='%s' num=%u sid=%llu port=%u", name, g_num, (unsigned long long)g_sid, g_port_base + g_num);
}

static unsigned sid_num(u64 sid) { return (unsigned)(sid - STEAMID_BASE); }

/* ---- queues ---- */
typedef struct Held { u64 from; int channel; int flags; int size; unsigned char *data; struct Held *next; } Held;
static Held *g_ready_head, *g_ready_tail; /* deliverable messages */
static Held *g_held_head, *g_held_tail;   /* from sessions not accepted yet */

#define MAX_SESS 64
static u64 g_accepted[MAX_SESS]; static int g_accepted_n;
static u64 g_requested[MAX_SESS]; static int g_requested_n;

static int has(u64 *arr, int n, u64 v) { for (int i = 0; i < n; i++) if (arr[i] == v) return 1; return 0; }
static void add_u64(u64 *arr, int *n, u64 v) { if (!has(arr, *n, v) && *n < MAX_SESS) arr[(*n)++] = v; }

static void push(Held **head, Held **tail, Held *h)
{
    h->next = NULL;
    if (*tail) (*tail)->next = h; else *head = h;
    *tail = h;
}

/* ---- callbacks ---- */
typedef struct Cb { int id; unsigned char data[400]; int size; struct Cb *next; } Cb;
static Cb *g_cb_head, *g_cb_tail;
static Cb *g_cb_current;

static void post_cb(int id, const void *data, int size)
{
    Cb *c = calloc(1, sizeof(Cb));
    c->id = id;
    if (size > (int)sizeof(c->data)) size = sizeof(c->data);
    c->size = size;
    memcpy(c->data, data, size);
    if (g_cb_tail) g_cb_tail->next = c; else g_cb_head = c;
    g_cb_tail = c;
}

#ifdef _WIN32
#pragma pack(push, 8)
#else
#pragma pack(push, 4) /* Steam packs its callback structs to 4 bytes on Linux */
#endif
typedef struct { int32_t user; int32_t callback; uint8_t *param; int32_t param_size; } CallbackMsg;
typedef struct { u64 call; int32_t callback; uint32_t param_size; } APICallCompleted;
typedef struct { int32_t result; u64 lobby; } LobbyCreatedCb;
typedef struct { u64 lobby; uint32_t perms; unsigned char locked; uint32_t response; } LobbyEnterCb;
typedef struct { u64 lobby, user_changed, making_change; uint32_t state; } LobbyChatUpdateCb;
#pragma pack(pop)

/* api call results */
typedef struct Res { u64 call; int cbid; unsigned char data[64]; int size; int done; struct Res *next; } Res;
static Res *g_res; static u64 g_call_counter = 1000;

static Res *new_res(int cbid)
{
    Res *r = calloc(1, sizeof(Res));
    r->call = ++g_call_counter; r->cbid = cbid; r->next = g_res; g_res = r;
    return r;
}
static void complete_res(Res *r, const void *data, int size)
{
    memcpy(r->data, data, size); r->size = size; r->done = 1;
    APICallCompleted c = { r->call, r->cbid, (uint32_t)size };
    post_cb(703, &c, sizeof(c));
}

/* ---- lobbies ---- */
static u64 g_my_lobby; /* lobby I own */
static u64 g_members[64]; static int g_member_n;
static u64 g_joined_lobby;
static Res *g_join_res;
static u64 g_join_lobby;
static DWORD g_join_sent;

/* ---- network ---- */
enum { PK_MSG = 1, PK_JOIN_REQ = 2, PK_JOIN_OK = 3, PK_JOIN_FAIL = 4, PK_LEAVE = 5 };
#pragma pack(push, 1)
typedef struct { uint32_t magic; uint8_t type; u64 from; u64 lobby; int32_t channel; int32_t flags; } PktHdr;
#pragma pack(pop)

static void net_start(void)
{
    if (g_sock != INVALID_SOCKET) return;
#ifdef _WIN32
    WSADATA w;
    if (!g_wsa) { WSAStartup(MAKEWORD(2, 2), &w); g_wsa = 1; }
#endif
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int rb = 16 * 1024 * 1024; setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (char *)&rb, sizeof(rb));
    int sb = 16 * 1024 * 1024; setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, (char *)&sb, sizeof(sb));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((u_short)(g_port_base + g_num));
    if (bind(g_sock, (struct sockaddr *)&a, sizeof(a)) != 0) logf_("bind failed");
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(g_sock, FIONBIO, &nb);
    /* ignore ICMP port unreachable resets on UDP (SIO_UDP_CONNRESET) */
    DWORD bytes; BOOL off = FALSE; WSAIoctl(g_sock, 0x9800000C, &off, sizeof(off), NULL, 0, &bytes, NULL, NULL);
#else
    fcntl(g_sock, F_SETFL, fcntl(g_sock, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static void net_send(u64 to, int type, u64 lobby, int channel, int flags, const void *data, int size)
{
    static unsigned char buf[70000];
    if (size > 65000) { logf_("send too large %d", size); return; }
    PktHdr h = { 0x4D4F434Bu, (uint8_t)type, g_sid, lobby, channel, flags };
    memcpy(buf, &h, sizeof(h));
    if (size) memcpy(buf + sizeof(h), data, size);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((u_short)(g_port_base + sid_num(to)));
    sendto(g_sock, (char *)buf, (int)(sizeof(h) + size), 0, (struct sockaddr *)&a, sizeof(a));
}

static void net_send(u64 to, int type, u64 lobby, int channel, int flags, const void *data, int size);
static void maybe_fake_invite(void)
{
    /* KFX_MOCK_STEAM_INVITE=<lobby id> or rp:<lobby id>: after 3 s act as if the user accepted an invite
       (GameLobbyJoinRequested_t) or clicked Join Game in the friends list (GameRichPresenceJoinRequested_t). */
    static int fired; static DWORD t0;
    const char *env = getenv("KFX_MOCK_STEAM_INVITE");
    if (fired || !env) return;
    if (!t0) t0 = GetTickCount();
    if (GetTickCount() - t0 < 3000) return;
    fired = 1;
    if (strncmp(env, "rp:", 3) == 0) {
        struct { u64 friend_id; char connect[256]; } rp;
        memset(&rp, 0, sizeof(rp)); rp.friend_id = STEAMID_BASE + 1;
        snprintf(rp.connect, sizeof(rp.connect), "+connect_lobby %s", env + 3);
        post_cb(337, &rp, sizeof(rp));
    } else {
        struct { u64 lobby, friend_id; } jr = { strtoull(env, NULL, 10), STEAMID_BASE + 1 };
        post_cb(333, &jr, sizeof(jr));
    }
    logf_("fake invite %s", env);
}

static void net_send(u64 to, int type, u64 lobby, int channel, int flags, const void *data, int size);
static void net_pump(void)
{
    static unsigned char buf[70000];
    if (g_sock == INVALID_SOCKET) return;
    maybe_fake_invite();
    /* the request travels over UDP and the owner may not be listening yet: ask again until it answers */
    if (g_join_res && !g_join_res->done && GetTickCount() - g_join_sent > 400) {
        g_join_sent = GetTickCount();
        net_send(STEAMID_BASE + (unsigned)(g_join_lobby - LOBBY_BASE), PK_JOIN_REQ, g_join_lobby, 0, 0, NULL, 0);
    }
    for (;;) {
        int n = recv(g_sock, (char *)buf, sizeof(buf), 0);
        if (n < (int)sizeof(PktHdr)) { if (n < 0 && NET_RESET()) continue; break; }
        PktHdr h; memcpy(&h, buf, sizeof(h));
        if (h.magic != 0x4D4F434Bu) continue;
        const unsigned char *payload = buf + sizeof(h); int psize = n - (int)sizeof(h);
        if (h.type == PK_MSG) {
            Held *m = calloc(1, sizeof(Held));
            m->from = h.from; m->channel = h.channel; m->flags = h.flags; m->size = psize;
            m->data = malloc(psize ? psize : 1); memcpy(m->data, payload, psize);
            if (has(g_accepted, g_accepted_n, h.from)) push(&g_ready_head, &g_ready_tail, m);
            else {
                push(&g_held_head, &g_held_tail, m);
                if (!has(g_requested, g_requested_n, h.from)) {
                    add_u64(g_requested, &g_requested_n, h.from);
                    unsigned char cb[200]; memset(cb, 0, sizeof(cb));
                    *(int32_t *)(cb + 0) = 16; *(int32_t *)(cb + 4) = 8; *(u64 *)(cb + 8) = h.from;
                    post_cb(1251, cb, 136);
                    logf_("session request from %llu", (unsigned long long)h.from);
                }
            }
        } else if (h.type == PK_JOIN_REQ) {
            if (g_my_lobby && h.lobby == g_my_lobby) {
                net_send(h.from, PK_JOIN_OK, h.lobby, 0, 0, NULL, 0);
                add_u64(g_members, &g_member_n, h.from);
                LobbyChatUpdateCb u = { g_my_lobby, h.from, h.from, 1 };
                post_cb(506, &u, sizeof(u));
            } else net_send(h.from, PK_JOIN_FAIL, h.lobby, 0, 0, NULL, 0);
        } else if (h.type == PK_JOIN_OK || h.type == PK_JOIN_FAIL) {
            if (g_join_res && !g_join_res->done) {
                LobbyEnterCb e = { h.lobby, 0, 0, h.type == PK_JOIN_OK ? 1u : 2u };
                if (h.type == PK_JOIN_OK) g_joined_lobby = h.lobby;
                post_cb(504, &e, sizeof(e));
                complete_res(g_join_res, &e, sizeof(e));
            }
        } else if (h.type == PK_LEAVE) {
            if (g_my_lobby && h.lobby == g_my_lobby) {
                for (int i = 0; i < g_member_n; i++) if (g_members[i] == h.from) { g_members[i] = g_members[--g_member_n]; break; }
                LobbyChatUpdateCb u = { g_my_lobby, h.from, h.from, 2 };
                post_cb(506, &u, sizeof(u));
            }
        }
    }
}

/* ---- exports: core ---- */
API bool SteamAPI_IsSteamRunning(void) { return 1; }
API int SteamAPI_InitFlat(char *err) { init_identity(); net_start(); g_inited = 1; if (err) err[0] = 0; logf_("InitFlat"); return 0; }
API int32_t SteamAPI_GetHSteamPipe(void) { return g_inited ? 7 : 0; }
API void SteamAPI_Shutdown(void) { logf_("Shutdown"); g_inited = 0; }
API void SteamAPI_ManualDispatch_Init(void) {}
API void SteamAPI_ManualDispatch_RunFrame(int32_t pipe) { net_pump(); }
API bool SteamAPI_ManualDispatch_GetNextCallback(int32_t pipe, CallbackMsg *msg)
{
    if (g_cb_current) return 0;
    if (!g_cb_head) return 0;
    g_cb_current = g_cb_head; g_cb_head = g_cb_head->next; if (!g_cb_head) g_cb_tail = NULL;
    msg->user = 1; msg->callback = g_cb_current->id; msg->param = g_cb_current->data; msg->param_size = g_cb_current->size;
    return 1;
}
API void SteamAPI_ManualDispatch_FreeLastCallback(int32_t pipe) { free(g_cb_current); g_cb_current = NULL; }
API bool SteamAPI_ManualDispatch_GetAPICallResult(int32_t pipe, u64 call, void *out, int size, int expected, bool *failed)
{
    for (Res *r = g_res; r; r = r->next) {
        if (r->call == call && r->done && r->cbid == expected) {
            memcpy(out, r->data, r->size < size ? r->size : size);
            if (failed) *failed = 0;
            return 1;
        }
    }
    if (failed) *failed = 1;
    return 0;
}

/* interfaces are opaque non-null pointers */
static char g_dummy[8];
API void *SteamAPI_SteamUser_v023(void) { return g_dummy + 0; }
API void *SteamAPI_SteamMatchmaking_v009(void) { return g_dummy + 1; }
API void *SteamAPI_SteamFriends_v018(void) { return g_dummy + 2; }
API void *SteamAPI_SteamNetworkingMessages_SteamAPI_v002(void) { return g_dummy + 3; }
API void *SteamAPI_SteamNetworkingUtils_SteamAPI_v004(void) { return g_dummy + 4; }
API u64 SteamAPI_ISteamUser_GetSteamID(void *self) { init_identity(); return g_sid; }
API bool SteamAPI_ISteamUser_BLoggedOn(void *self) { return 1; }
API bool SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32(void *self, int v, int32_t x) { return 1; }

/* ---- lobbies ---- */
API u64 SteamAPI_ISteamMatchmaking_CreateLobby(void *self, int type, int max)
{
    init_identity();
    g_my_lobby = LOBBY_BASE + g_num;
    LobbyCreatedCb c = { 1, g_my_lobby };
    Res *r = new_res(513);
    complete_res(r, &c, sizeof(c));
    LobbyEnterCb e = { g_my_lobby, 0, 0, 1 };
    post_cb(504, &e, sizeof(e));
    logf_("CreateLobby -> %llu", (unsigned long long)g_my_lobby);
    return r->call;
}
API u64 SteamAPI_ISteamMatchmaking_JoinLobby(void *self, u64 lobby)
{
    g_join_res = new_res(504);
    u64 owner = STEAMID_BASE + (unsigned)(lobby - LOBBY_BASE);
    logf_("JoinLobby %llu owner %llu", (unsigned long long)lobby, (unsigned long long)owner);
    g_join_lobby = lobby; g_join_sent = GetTickCount();
    net_send(owner, PK_JOIN_REQ, lobby, 0, 0, NULL, 0);
    return g_join_res->call;
}
API void SteamAPI_ISteamMatchmaking_LeaveLobby(void *self, u64 lobby)
{
    if (lobby == g_joined_lobby) { net_send(STEAMID_BASE + (unsigned)(lobby - LOBBY_BASE), PK_LEAVE, lobby, 0, 0, NULL, 0); g_joined_lobby = 0; }
    if (lobby == g_my_lobby) { g_my_lobby = 0; g_member_n = 0; }
}
API bool SteamAPI_ISteamMatchmaking_SetLobbyData(void *self, u64 lobby, const char *k, const char *v) { return 1; }
API bool SteamAPI_ISteamMatchmaking_SetLobbyJoinable(void *self, u64 lobby, bool j) { return 1; }
API u64 SteamAPI_ISteamMatchmaking_GetLobbyOwner(void *self, u64 lobby) { return STEAMID_BASE + (unsigned)(lobby - LOBBY_BASE); }
API bool SteamAPI_ISteamFriends_SetRichPresence(void *self, const char *k, const char *v) { logf_("RichPresence %s=%s", k ? k : "(null)", v ? v : "(null)"); return 1; }
API void SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(void *self, u64 lobby) { logf_("InviteDialog lobby %llu", (unsigned long long)lobby); }

/* ---- friends (HXCoop) ---- */
#pragma pack(push, 8)
typedef struct { u64 game_id; uint32_t ip; uint16_t port, query_port; u64 lobby; } FriendGameInfoMock;
#pragma pack(pop)
static const char *num_name(unsigned num)
{
    static char buf[4][32]; static int k;
    char *b = buf[k++ & 3];
    if (num == 1) return "Host";
    if (num == 2) return "Client";
    snprintf(b, 32, "User%u", num);
    return b;
}
API int SteamAPI_ISteamFriends_GetFriendCount(void *self, int flags) { init_identity(); return g_num == 1 ? 0 : 1; }
API u64 SteamAPI_ISteamFriends_GetFriendByIndex(void *self, int i, int flags) { return STEAMID_BASE + 1; }
API bool SteamAPI_ISteamFriends_GetFriendGamePlayed(void *self, u64 sid, FriendGameInfoMock *gi)
{
    if (sid != STEAMID_BASE + 1) return 0;
    memset(gi, 0, sizeof(*gi));
    gi->game_id = 480; gi->lobby = LOBBY_BASE + 1;
    return 1;
}
API const char *SteamAPI_ISteamFriends_GetFriendPersonaName(void *self, u64 sid) { return num_name(sid_num(sid)); }
API const char *SteamAPI_ISteamFriends_GetPersonaName(void *self) { init_identity(); return num_name(g_num); }
API const char *SteamAPI_ISteamMatchmaking_GetLobbyData(void *self, u64 lobby, const char *k) { return k && !strcmp(k, "ootsteam") ? "1" : (k && !strcmp(k, "ver") ? "1" : ""); }
API void SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess(void *self) {}
API int SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(void *self, u64 lobby) { return lobby == g_my_lobby ? g_member_n + 1 : 0; }
API u64 SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(void *self, u64 lobby, int i)
{
    if (lobby != g_my_lobby) return 0;
    if (i == 0) return g_sid;
    return i - 1 < g_member_n ? g_members[i - 1] : 0;
}

/* ---- identity helpers: type int32, size int32, steamid at +8 ---- */
API void SteamAPI_SteamNetworkingIdentity_SetSteamID64(void *ident, u64 sid)
{
    *(int32_t *)((char *)ident + 0) = 16; *(int32_t *)((char *)ident + 4) = 8; memcpy((char *)ident + 8, &sid, 8);
}
API u64 SteamAPI_SteamNetworkingIdentity_GetSteamID64(const void *ident)
{
    u64 sid = 0;
    if (*(const int32_t *)((const char *)ident + 0) == 16 && *(const int32_t *)((const char *)ident + 4) == 8) memcpy(&sid, (const char *)ident + 8, 8);
    return sid;
}

/* ---- messages: struct { void *data; int size at 8; uint32 conn at 12; identity[136] at offset 16 } (64 bit) ---- */
API int SteamAPI_ISteamNetworkingMessages_SendMessageToUser(void *self, const void *ident, const void *data, uint32_t size, int flags, int channel)
{
    u64 to = SteamAPI_SteamNetworkingIdentity_GetSteamID64(ident);
    if (!to) return 2;
    add_u64(g_accepted, &g_accepted_n, to); /* contacting someone accepts the session implicitly */
    net_send(to, PK_MSG, 0, channel, flags, data, (int)size);
    return 1;
}

API int SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(void *self, int channel, void **out, int max)
{
    net_pump();
    int n = 0;
    Held **pp = &g_ready_head, *prev = NULL;
    while (*pp && n < max) {
        Held *h = *pp;
        if (h->channel != channel) { prev = h; pp = &h->next; continue; }
        unsigned char *blob = calloc(1, 256 + h->size);
        void *dp = blob + 256; memcpy(dp, h->data, h->size);
        memcpy(blob + 0, &dp, sizeof(dp)); *(int32_t *)(blob + 8) = h->size;
        SteamAPI_SteamNetworkingIdentity_SetSteamID64(blob + 16, h->from);
        out[n++] = blob;
        *pp = h->next; if (g_ready_tail == h) g_ready_tail = prev;
        free(h->data); free(h);
    }
    return n;
}
API bool SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(void *self, const void *ident)
{
    u64 who = SteamAPI_SteamNetworkingIdentity_GetSteamID64(ident);
    if (!who) return 0;
    add_u64(g_accepted, &g_accepted_n, who);
    Held **pp = &g_held_head, *prev = NULL;
    while (*pp) {
        Held *h = *pp;
        if (h->from != who) { prev = h; pp = &h->next; continue; }
        *pp = h->next; if (g_held_tail == h) g_held_tail = prev;
        push(&g_ready_head, &g_ready_tail, h);
    }
    logf_("accepted %llu", (unsigned long long)who);
    return 1;
}
API bool SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(void *self, const void *ident)
{
    u64 who = SteamAPI_SteamNetworkingIdentity_GetSteamID64(ident);
    for (int i = 0; i < g_accepted_n; i++) if (g_accepted[i] == who) { g_accepted[i] = g_accepted[--g_accepted_n]; break; }
    for (int i = 0; i < g_requested_n; i++) if (g_requested[i] == who) { g_requested[i] = g_requested[--g_requested_n]; break; }
    return 1;
}
API void SteamAPI_SteamNetworkingMessage_t_Release(void *msg) { free(msg); }

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { return TRUE; }
#endif
