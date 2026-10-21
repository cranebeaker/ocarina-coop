/*
 * OOTSteamLink: Steam ("Spacewar", AppID 480) relay for the Ocarina of Time online co-op.
 *
 * Players never deal with IP addresses or port forwarding:
 *  - host:  starts OOTServer.exe, creates a friends-only Steam lobby and relays every friend's
 *           TCP stream between Steam and the local server port. Then starts the host's own game
 *           pointed straight at the local server.
 *  - join:  finds a Steam friend who hosts (or takes a lobby id / invite), listens on a local TCP
 *           port and relays it to the host over Steam. Then starts the game pointed at that port.
 *
 * The game talks TCP to OOTServer (default port 21050). Each local TCP connection becomes one
 * relayed stream with a 32 bit id. Steam messages on channel 0 are reliable and ordered, so a
 * stream is cut into chunks of up to CHUNK bytes and rebuilt in order on the other side.
 *   frame = type(1) id(4, little endian) payload
 *   'O' open stream, 'D' data, 'C' close stream
 * Channel 1 carries a 1 s keep-alive ('K') and a goodbye ('B').
 *
 * steam_api64.dll is loaded at runtime through the flat C API (no SDK headers), see
 * spacewar.md. Needs steam_appid.txt = 480 next to the exe (written if missing).
 *
 *   OOTSteamLink.exe                    menu
 *   OOTSteamLink.exe host               host
 *   OOTSteamLink.exe join [lobby id]    join (no id: a friend who is hosting)
 * Options: --port <server port, 21050>   --server-exe <path>   --game-exe <path>
 *          --local-port <join listen port, 0 = any free port>
 *          --no-server (relay to a server that is already running)
 *          --no-game / --no-launch (do not start soh.exe)
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>

typedef uint64_t u64;
typedef uint32_t u32;

#define PROTO_VERSION "1"
#define CHUNK 16000
#define PEER_TIMEOUT_MS 20000

/* ---------------------------------------------------------------- steam flat api */

typedef struct { int32_t user; int32_t callback; uint8_t *param; int32_t param_size; } CallbackMsg;
#pragma pack(push, 8)
typedef struct { u64 call; int32_t callback; uint32_t param_size; } APICallCompleted;
typedef struct { int32_t result; u64 lobby; } LobbyCreated;
typedef struct { u64 lobby; uint32_t perms; bool locked; uint32_t response; } LobbyEnter;
typedef struct { u64 lobby, user_changed, making_change; uint32_t state; } LobbyChatUpdate;
typedef struct { u64 lobby, friend_id; } GameLobbyJoinRequested;
typedef struct { u64 friend_id; char connect[256]; } GameRichPresenceJoinRequested;
typedef struct { u64 game_id; uint32_t ip; uint16_t port, query_port; u64 lobby; } FriendGameInfo;
#pragma pack(pop)

/* Start of a received SteamNetworkingMessage_t; the sender identity sits at offset 16. */
typedef struct { void *data; int size; uint32_t conn; } MsgHead;

#define CB_LOBBY_JOIN_REQUESTED 333
#define CB_RICH_PRESENCE_JOIN   337
#define CB_LOBBY_ENTER          504
#define CB_LOBBY_CHAT_UPDATE    506
#define CB_LOBBY_CREATED        513
#define CB_CALL_COMPLETED       703
#define CB_SESSION_REQUEST      1251
#define CB_SESSION_FAILED       1252

#define SEND_NO_NAGLE 1
#define SEND_RELIABLE 8
#define IDENT_SIZE    160 /* real size 136, kept opaque */

static struct {
    bool (__cdecl *IsSteamRunning)(void);
    int (__cdecl *InitFlat)(char *err);
    void (__cdecl *Shutdown)(void);
    int32_t (__cdecl *GetHSteamPipe)(void);
    void (__cdecl *DispatchInit)(void);
    void (__cdecl *RunFrame)(int32_t);
    bool (__cdecl *GetNextCallback)(int32_t, CallbackMsg *);
    void (__cdecl *FreeLastCallback)(int32_t);
    bool (__cdecl *GetAPICallResult)(int32_t, u64, void *, int, int, bool *);
    void *(__cdecl *User)(void);
    void *(__cdecl *Matchmaking)(void);
    void *(__cdecl *Friends)(void);
    void *(__cdecl *Messages)(void);
    void *(__cdecl *Utils)(void);
    u64 (__cdecl *GetSteamID)(void *);
    u64 (__cdecl *CreateLobby)(void *, int, int);
    u64 (__cdecl *JoinLobby)(void *, u64);
    void (__cdecl *LeaveLobby)(void *, u64);
    bool (__cdecl *SetLobbyData)(void *, u64, const char *, const char *);
    const char *(__cdecl *GetLobbyData)(void *, u64, const char *);
    u64 (__cdecl *GetLobbyOwner)(void *, u64);
    int (__cdecl *GetNumLobbyMembers)(void *, u64);
    u64 (__cdecl *GetLobbyMemberByIndex)(void *, u64, int);
    bool (__cdecl *SetRichPresence)(void *, const char *, const char *);
    void (__cdecl *InviteDialog)(void *, u64);
    int (__cdecl *GetFriendCount)(void *, int);
    u64 (__cdecl *GetFriendByIndex)(void *, int, int);
    bool (__cdecl *GetFriendGamePlayed)(void *, u64, FriendGameInfo *);
    const char *(__cdecl *GetFriendPersonaName)(void *, u64);
    const char *(__cdecl *GetPersonaName)(void *);
    void (__cdecl *IdentSet)(void *, u64);
    u64 (__cdecl *IdentGet)(const void *);
    int (__cdecl *SendMessageToUser)(void *, const void *, const void *, uint32_t, int, int);
    int (__cdecl *ReceiveMessages)(void *, int, void **, int);
    bool (__cdecl *AcceptSession)(void *, const void *);
    bool (__cdecl *CloseSession)(void *, const void *);
    void (__cdecl *MessageRelease)(void *);
    bool (__cdecl *SetConfigInt)(void *, int, int32_t);
    void (__cdecl *InitRelay)(void *);
} S;

static void *g_user, *g_mm, *g_friends, *g_msgs, *g_utils;
static int32_t g_pipe;
static u64 g_me;

/* Keyboard only when stdin is a real console (tests and services run without one). */
static volatile LONG g_stop, g_done; /* set by Ctrl+C / Ctrl+Break / closing the window */
static BOOL WINAPI on_ctrl(DWORD type)
{
    InterlockedExchange(&g_stop, 1);
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
        for (int i = 0; i < 150 && !g_done; i++) Sleep(100); /* give the main loop time to stop the server cleanly */
    return TRUE;
}

static bool g_console;
static bool key_hit(void) { return g_console && _kbhit(); }
static int key_get(void) { return g_console ? _getch() : -1; }
static bool g_quiet_fail; /* tests: do not wait for a key press on errors */

static void fail(const char *msg)
{
    printf("\n%s\n", msg);
    if (!g_quiet_fail) { printf("\nPress a key to close.\n"); key_get(); }
    exit(1);
}

static FARPROC need(HMODULE m, const char *name)
{
    FARPROC p = GetProcAddress(m, name);
    if (!p) {
        char buf[300];
        snprintf(buf, sizeof(buf), "steam_api64.dll is missing %s (too old?).", name);
        fail(buf);
    }
    return p;
}

#define LOAD(field, name) *(FARPROC *)&S.field = need(m, name)
#define LOAD_OPT(field, name) *(FARPROC *)&S.field = GetProcAddress(m, name)

static void exe_dir(char *out, size_t size)
{
    char *slash;
    GetModuleFileNameA(NULL, out, (DWORD)size);
    if ((slash = strrchr(out, '\\'))) *slash = 0;
}

static void steam_start(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    exe_dir(dir, sizeof(dir));
    SetCurrentDirectoryA(dir); /* steam_appid.txt and the DLL live next to the exe */
    snprintf(path, sizeof(path), "%s\\steam_appid.txt", dir);
    FILE *f = fopen(path, "r");
    if (f) fclose(f);
    else if ((f = fopen(path, "w"))) { fputs("480", f); fclose(f); }

    snprintf(path, sizeof(path), "%s\\steam_api64.dll", dir);
    HMODULE m = LoadLibraryA(path);
    if (!m) fail("steam_api64.dll not found next to OOTSteamLink.exe.");
    LOAD(IsSteamRunning, "SteamAPI_IsSteamRunning");
    LOAD(InitFlat, "SteamAPI_InitFlat");
    LOAD(Shutdown, "SteamAPI_Shutdown");
    LOAD(GetHSteamPipe, "SteamAPI_GetHSteamPipe");
    LOAD(DispatchInit, "SteamAPI_ManualDispatch_Init");
    LOAD(RunFrame, "SteamAPI_ManualDispatch_RunFrame");
    LOAD(GetNextCallback, "SteamAPI_ManualDispatch_GetNextCallback");
    LOAD(FreeLastCallback, "SteamAPI_ManualDispatch_FreeLastCallback");
    LOAD(GetAPICallResult, "SteamAPI_ManualDispatch_GetAPICallResult");
    LOAD(User, "SteamAPI_SteamUser_v023");
    LOAD(Matchmaking, "SteamAPI_SteamMatchmaking_v009");
    LOAD(Friends, "SteamAPI_SteamFriends_v018");
    LOAD(Messages, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    LOAD(Utils, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    LOAD(GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
    LOAD(CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
    LOAD(JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
    LOAD(LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    LOAD(SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    LOAD(GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    LOAD(GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    LOAD(GetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    LOAD(GetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    LOAD(SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence");
    LOAD(InviteDialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    LOAD(GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount");
    LOAD(GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex");
    LOAD(GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    LOAD(GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    LOAD(GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
    LOAD(IdentSet, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    LOAD(IdentGet, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    LOAD(SendMessageToUser, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    LOAD(ReceiveMessages, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    LOAD(AcceptSession, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    LOAD(CloseSession, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    LOAD(MessageRelease, "SteamAPI_SteamNetworkingMessage_t_Release");
    LOAD(SetConfigInt, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32");
    LOAD_OPT(InitRelay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");

    if (!S.IsSteamRunning()) fail("Steam is not running. Start Steam, log in, and try again.");
    char err[1024] = "";
    int r = S.InitFlat(err);
    if (r != 0) {
        char buf[1200];
        snprintf(buf, sizeof(buf), "Could not connect to Steam (code %d): %s", r, err);
        fail(buf);
    }
    S.DispatchInit();
    g_pipe = S.GetHSteamPipe();
    g_user = S.User(); g_mm = S.Matchmaking(); g_friends = S.Friends();
    g_msgs = S.Messages(); g_utils = S.Utils();
    g_me = S.GetSteamID(g_user);
    if (S.InitRelay) S.InitRelay(g_utils);
}

static void ident(void *buf, u64 sid) { memset(buf, 0, IDENT_SIZE); S.IdentSet(buf, sid); }

static u64 msg_sender(void *msg) { return S.IdentGet((char *)msg + 16); }

static void accept_session(u64 sid)
{
    unsigned char id[IDENT_SIZE];
    ident(id, sid);
    S.AcceptSession(g_msgs, id);
}

static void close_session(u64 sid)
{
    unsigned char id[IDENT_SIZE];
    ident(id, sid);
    S.CloseSession(g_msgs, id);
}

static const char *friend_name(u64 sid)
{
    const char *n = S.GetFriendPersonaName(g_friends, sid);
    return n && *n ? n : "?";
}

/* ---------------------------------------------------------------- callbacks */

typedef void (*CbHandler)(int id, void *data);

static void pump(CbHandler h)
{
    CallbackMsg msg;
    S.RunFrame(g_pipe);
    while (S.GetNextCallback(g_pipe, &msg)) {
        if (msg.callback == CB_CALL_COMPLETED) {
            APICallCompleted *done = (APICallCompleted *)msg.param;
            void *buf = calloc(1, done->param_size + 16);
            bool failed = true;
            if (S.GetAPICallResult(g_pipe, done->call, buf, done->param_size, done->callback, &failed) && !failed)
                h(done->callback, buf);
            free(buf);
        } else
            h(msg.callback, msg.param);
        S.FreeLastCallback(g_pipe);
    }
}

static void relay_config(void)
{
    /* k_ESteamNetworkingConfig_SendBufferSize 9, RecvBufferSize 47 */
    S.SetConfigInt(g_utils, 9, 8 << 20);
    S.SetConfigInt(g_utils, 47, 8 << 20);
}

/* ---------------------------------------------------------------- options, launching */

static char g_server_exe[MAX_PATH], g_game_exe[MAX_PATH];
static int g_port = 21050, g_local_port;
static bool g_no_server, g_no_game;
static HANDLE g_server, g_game;
static DWORD g_server_pid;

static void find_exe(char *out, const char *name, const char *subdirs[])
{
    char dir[MAX_PATH];
    exe_dir(dir, sizeof(dir));
    for (int i = 0; subdirs[i]; i++) {
        char p[MAX_PATH], full[MAX_PATH];
        snprintf(p, sizeof(p), "%s\\%s%s%s", dir, subdirs[i], subdirs[i][0] ? "\\" : "", name);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
            GetFullPathNameA(p, sizeof(full), full, NULL);
            snprintf(out, MAX_PATH, "%s", full);
            return;
        }
    }
}

static HANDLE spawn(const char *exe, const char *args, DWORD flags, DWORD *pid, const char *logname)
{
    char cmd[2048], cwd[MAX_PATH], *slash;
    snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, args);
    snprintf(cwd, sizeof(cwd), "%s", exe);
    if ((slash = strrchr(cwd, '\\'))) *slash = 0;
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    HANDLE log = INVALID_HANDLE_VALUE;
    if (logname) { /* send the child's console output to a log file in its folder */
        char path[MAX_PATH];
        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
        snprintf(path, sizeof(path), "%s\\%s", cwd, logname);
        log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, 0, NULL);
        if (log != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = si.hStdError = log;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            SetHandleInformation(si.hStdInput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
    }
    BOOL ok = CreateProcessA(exe, cmd, NULL, NULL, log != INVALID_HANDLE_VALUE, flags, NULL, cwd, &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        char buf[MAX_PATH + 100];
        snprintf(buf, sizeof(buf), "Could not start %s (error %lu).", exe, GetLastError());
        fail(buf);
    }
    CloseHandle(pi.hThread);
    if (pid) *pid = pi.dwProcessId;
    return pi.hProcess;
}

static bool tcp_port_open(int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((u_short)port);
    bool ok = connect(s, (struct sockaddr *)&a, sizeof(a)) == 0;
    closesocket(s);
    return ok;
}

static void start_server(void)
{
    if (g_no_server) return;
    if (!g_server_exe[0]) {
        const char *tries[] = { "", "server\\", "..\\server\\", "..\\server\\build\\", NULL };
        find_exe(g_server_exe, "OOTServer.exe", tries);
    }
    if (!g_server_exe[0]) fail("OOTServer.exe not found. Put it next to OOTSteamLink.exe or use --server-exe <path>.");
    if (tcp_port_open(g_port)) {
        char buf[200];
        snprintf(buf, sizeof(buf), "Port %d is already in use. Close the other server or use --port <number>.", g_port);
        fail(buf);
    }
    char args[64];
    snprintf(args, sizeof(args), "--port %d", g_port);
    /* own process group so Ctrl+Break can stop it cleanly (it saves team progress on exit) */
    g_server = spawn(g_server_exe, args, CREATE_NEW_PROCESS_GROUP, &g_server_pid, "OOTServer.log");
    /* safety net: if this tool is killed, the server goes with it */
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
        memset(&info, 0, sizeof(info));
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        AssignProcessToJobObject(job, g_server);
    }
    for (int i = 0; i < 100; i++) {
        if (tcp_port_open(g_port)) { printf("Server running on port %d.\n", g_port); return; }
        if (WaitForSingleObject(g_server, 0) == WAIT_OBJECT_0) fail("OOTServer.exe stopped right after starting.");
        Sleep(100);
    }
    fail("OOTServer.exe did not open its port.");
}

static void stop_server(void)
{
    if (!g_server) return;
    if (WaitForSingleObject(g_server, 0) != WAIT_OBJECT_0) {
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, g_server_pid);
        if (WaitForSingleObject(g_server, 10000) != WAIT_OBJECT_0) TerminateProcess(g_server, 1);
    }
    CloseHandle(g_server);
    g_server = NULL;
}

/* The game is started by a second copy of this program that is spawned before Steam is initialised. Once
   SteamAPI_Init has run, every child of this process inherits Steam's overlay and its settings for AppID 480,
   including SDL_GAMECONTROLLER_IGNORE_DEVICES, which makes SDL in the game ignore every real controller. */
static HANDLE g_launcher, g_launcher_in;

static void start_launcher(void)
{
    if (g_no_game) return;
    char self[MAX_PATH], cmd[MAX_PATH + 32];
    GetModuleFileNameA(NULL, self, sizeof(self));
    snprintf(cmd, sizeof(cmd), "\"%s\" --launcher", self);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    SetHandleInformation(si.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(si.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    BOOL ok = CreateProcessA(self, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CloseHandle(rd);
    if (!ok) { CloseHandle(wr); return; } /* start_game falls back to starting the game itself */
    CloseHandle(pi.hThread);
    g_launcher = pi.hProcess;
    g_launcher_in = wr;
}

/* --launcher: wait for the game path and server address, start the game, exit when it closes. */
static int run_launcher(void)
{
    char exe[MAX_PATH], addr[128];
    SetConsoleCtrlHandler(NULL, TRUE); /* Ctrl+C is for the main window, not for us */
    if (!fgets(exe, sizeof(exe), stdin) || !fgets(addr, sizeof(addr), stdin)) return 0; /* no game was started */
    exe[strcspn(exe, "\r\n")] = 0;
    addr[strcspn(addr, "\r\n")] = 0;
    SetEnvironmentVariableA("OOT_SERVER", addr); /* the game connects here instead of its saved server */
    HANDLE game = spawn(exe, "", 0, NULL, NULL);
    DWORD code = 0;
    WaitForSingleObject(game, INFINITE);
    GetExitCodeProcess(game, &code);
    return (int)code;
}

static void start_game(const char *host, int port)
{
    if (g_no_game) return;
    if (!g_game_exe[0]) {
        const char *tries[] = { "", "..\\", NULL };
        find_exe(g_game_exe, "soh.exe", tries);
    }
    if (!g_game_exe[0]) {
        printf("soh.exe not found next to OOTSteamLink.exe: start the game yourself.\n"
               "Set the environment variable OOT_SERVER=%s:%d for it, or enter that host and port in the online window.\n",
               host, port);
        return;
    }
    char addr[128];
    snprintf(addr, sizeof(addr), "%s:%d", host, port);
    if (g_launcher) {
        char line[MAX_PATH + 160];
        DWORD n;
        int len = snprintf(line, sizeof(line), "%s\n%s\n", g_game_exe, addr);
        WriteFile(g_launcher_in, line, len, &n, NULL);
        CloseHandle(g_launcher_in);
        g_launcher_in = NULL;
        g_game = g_launcher; /* the launcher exits when the game does */
    } else {
        SetEnvironmentVariableA("OOT_SERVER", addr); /* the game connects here instead of its saved server */
        g_game = spawn(g_game_exe, "", 0, NULL, NULL);
    }
    printf("Started the game (server %s).\n", addr);
}

static bool game_exited(void) { return g_game && WaitForSingleObject(g_game, 0) == WAIT_OBJECT_0; }

/* ---------------------------------------------------------------- relayed streams */

typedef struct Link {
    u64 peer;          /* the remote Steam user */
    u32 id;            /* stream id chosen by the joining side */
    SOCKET s;          /* local TCP socket, INVALID_SOCKET when none */
    unsigned char *out; size_t outlen, outcap; /* data waiting for the local socket */
    bool closing;      /* remote closed: close the socket once the output is flushed */
    bool used;
} Link;

#define MAX_LINKS 256
static Link g_links[MAX_LINKS];
static unsigned long long g_bytes_up, g_bytes_down; /* local game -> Steam, Steam -> local game */

static Link *find_link(u64 peer, u32 id)
{
    for (int i = 0; i < MAX_LINKS; i++)
        if (g_links[i].used && g_links[i].peer == peer && g_links[i].id == id) return &g_links[i];
    return NULL;
}

static Link *new_link(u64 peer, u32 id, SOCKET s)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        Link *l = &g_links[i];
        if (l->used) continue;
        memset(l, 0, sizeof(*l));
        l->used = true; l->peer = peer; l->id = id; l->s = s;
        return l;
    }
    return NULL;
}

static void free_link(Link *l)
{
    if (l->s != INVALID_SOCKET) closesocket(l->s);
    free(l->out);
    memset(l, 0, sizeof(*l));
}

static void set_nonblocking(SOCKET s)
{
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));
}

static CbHandler g_cb; /* callback handler used while waiting inside a send */

/* Reliable send of one frame. Retries while Steam's send buffer is full. */
static bool send_frame(u64 peer, char type, u32 id, const void *data, int len)
{
    static unsigned char buf[CHUNK + 16];
    unsigned char idb[IDENT_SIZE];
    if (len > CHUNK) return false;
    buf[0] = (unsigned char)type;
    memcpy(buf + 1, &id, 4);
    if (len) memcpy(buf + 5, data, len);
    ident(idb, peer);
    for (int tries = 0; tries < 1500; tries++) {
        int r = S.SendMessageToUser(g_msgs, idb, buf, (uint32_t)(len + 5), SEND_RELIABLE | SEND_NO_NAGLE, 0);
        if (r == 1) return true;
        if (r != 25 /* limit exceeded: send buffer full */) return false;
        Sleep(2);
        if (g_cb) pump(g_cb);
    }
    return false;
}

static void send_control(u64 peer, char c)
{
    unsigned char idb[IDENT_SIZE];
    ident(idb, peer);
    S.SendMessageToUser(g_msgs, idb, &c, 1, SEND_RELIABLE | SEND_NO_NAGLE, 1);
}

static void close_link(Link *l, bool tell)
{
    if (!l || !l->used) return; /* a callback pumped during a send may have dropped it already */
    if (tell) send_frame(l->peer, 'C', l->id, NULL, 0);
    free_link(l);
}

static void link_append(Link *l, const unsigned char *data, int len)
{
    if (l->outlen + len > l->outcap) {
        size_t cap = l->outcap ? l->outcap : 4096;
        while (cap < l->outlen + len) cap *= 2;
        l->out = realloc(l->out, cap);
        l->outcap = cap;
    }
    memcpy(l->out + l->outlen, data, len);
    l->outlen += len;
}

/* Write what is queued for the local socket. Returns false if the socket failed. */
static bool link_flush(Link *l)
{
    while (l->outlen > 0 && l->s != INVALID_SOCKET) {
        int n = send(l->s, (char *)l->out, (int)(l->outlen > 65536 ? 65536 : l->outlen), 0);
        if (n > 0) {
            memmove(l->out, l->out + n, l->outlen - n);
            l->outlen -= n;
        } else if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;
        else return false;
    }
    return true;
}

/* Local socket -> Steam for every link. */
static void links_read_local(void)
{
    static unsigned char buf[CHUNK];
    for (int i = 0; i < MAX_LINKS; i++) {
        Link *l = &g_links[i];
        if (!l->used) continue;
        if (!link_flush(l)) { close_link(l, true); continue; }
        if (l->closing) {
            if (l->outlen == 0) free_link(l);
            continue;
        }
        for (int round = 0; round < 8; round++) {
            int got = recv(l->s, (char *)buf, sizeof(buf), 0);
            if (got > 0) {
                g_bytes_up += got;
                if (!send_frame(l->peer, 'D', l->id, buf, got)) { close_link(l, true); break; }
                if (!l->used) break;
            } else if (got == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
                close_link(l, true); /* local game closed the connection */
                break;
            } else break;
        }
    }
}

static void close_links_of(u64 peer)
{
    for (int i = 0; i < MAX_LINKS; i++)
        if (g_links[i].used && g_links[i].peer == peer) free_link(&g_links[i]);
}

/* ---------------------------------------------------------------- host */

#define MAX_PEERS 16
typedef struct { u64 sid; DWORD last_heard, last_ping; } Peer;
static Peer g_peers[MAX_PEERS];
static u64 g_lobby;
static bool g_lobby_failed;

static Peer *find_peer(u64 sid)
{
    for (int i = 0; i < MAX_PEERS; i++) if (g_peers[i].sid == sid) return &g_peers[i];
    return NULL;
}

static bool lobby_has_member(u64 sid)
{
    int n = S.GetNumLobbyMembers(g_mm, g_lobby);
    for (int i = 0; i < n; i++) if (S.GetLobbyMemberByIndex(g_mm, g_lobby, i) == sid) return true;
    return false;
}

static void drop_peer(Peer *p, const char *why)
{
    if (!p || !p->sid) return;
    printf("%s left (%s).\n", friend_name(p->sid), why);
    close_links_of(p->sid);
    close_session(p->sid);
    memset(p, 0, sizeof(*p));
}

static Peer *add_peer(u64 sid)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = &g_peers[i];
        if (p->sid) continue;
        memset(p, 0, sizeof(*p));
        p->sid = sid; p->last_heard = p->last_ping = GetTickCount();
        printf("%s connected through Steam.\n", friend_name(sid));
        return p;
    }
    return NULL;
}

static void host_cb(int id, void *data)
{
    switch (id) {
    case CB_LOBBY_CREATED: {
        LobbyCreated *c = data;
        if (c->result != 1) { g_lobby_failed = true; break; }
        g_lobby = c->lobby;
        char buf[64];
        S.SetLobbyData(g_mm, g_lobby, "ootsteam", "1");
        S.SetLobbyData(g_mm, g_lobby, "ver", PROTO_VERSION);
        S.SetLobbyData(g_mm, g_lobby, "host", S.GetPersonaName(g_friends));
        snprintf(buf, sizeof(buf), "+connect_lobby %llu", (unsigned long long)g_lobby);
        S.SetRichPresence(g_friends, "connect", buf);
        S.SetRichPresence(g_friends, "status", "Hosting Ocarina of Time co-op");
        break;
    }
    case CB_SESSION_REQUEST: {
        u64 sid = S.IdentGet(data);
        if (g_lobby && lobby_has_member(sid)) accept_session(sid); /* only people who joined the lobby */
        break;
    }
    case CB_SESSION_FAILED: {
        u64 sid = S.IdentGet(data);
        drop_peer(find_peer(sid), "connection failed");
        break;
    }
    case CB_LOBBY_CHAT_UPDATE: {
        LobbyChatUpdate *u = data;
        if (u->state & (0x2 | 0x4 | 0x8 | 0x10)) drop_peer(find_peer(u->user_changed), "left the lobby");
        break;
    }
    }
}

static void host_handle_frame(Peer *p, const unsigned char *d, int size)
{
    u32 id;
    if (size < 5) return;
    memcpy(&id, d + 1, 4);
    Link *l = find_link(p->sid, id);
    switch (d[0]) {
    case 'O': {
        if (l) close_link(l, false);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in a; memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((u_short)g_port);
        if (s == INVALID_SOCKET || connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
            if (s != INVALID_SOCKET) closesocket(s);
            printf("Could not reach the local server on port %d.\n", g_port);
            send_frame(p->sid, 'C', id, NULL, 0);
            break;
        }
        set_nonblocking(s);
        if (!new_link(p->sid, id, s)) { closesocket(s); send_frame(p->sid, 'C', id, NULL, 0); }
        break;
    }
    case 'D':
        if (l) { link_append(l, d + 5, size - 5); g_bytes_down += size - 5; }
        break;
    case 'C':
        if (l) { if (l->outlen) l->closing = true; else free_link(l); }
        break;
    }
}

static int run_host(void)
{
    start_server();
    S.CreateLobby(g_mm, 1 /* friends only */, MAX_PEERS + 1);
    printf("Creating a friends-only Steam lobby...\n");
    DWORD t0 = GetTickCount();
    while (!g_lobby && !g_lobby_failed && GetTickCount() - t0 < 15000) { pump(host_cb); Sleep(20); }
    if (!g_lobby) { stop_server(); fail("Steam did not create a lobby. Check that you are online in Steam."); }

    start_game("127.0.0.1", g_port);
    printf("\nHosting. Friends join by running OOTSteamLink and choosing Join, or by right-clicking you in\n"
           "the Steam friends list and choosing Join Game (with OOTSteamLink already open on their side).\n"
           "Lobby id: %llu\nKeep this window open while you play. Press Q to stop hosting.\n\n",
           (unsigned long long)g_lobby);
    g_cb = host_cb;

    for (;;) {
        pump(host_cb);
        void *msgs[64];
        int n;
        while ((n = S.ReceiveMessages(g_msgs, 0, msgs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                u64 sid = msg_sender(msgs[i]);
                MsgHead *h = msgs[i];
                Peer *p = find_peer(sid);
                if (!p && lobby_has_member(sid)) p = add_peer(sid);
                if (p) {
                    p->last_heard = GetTickCount();
                    host_handle_frame(p, h->data, h->size);
                }
                S.MessageRelease(msgs[i]);
            }
        }
        while ((n = S.ReceiveMessages(g_msgs, 1, msgs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                MsgHead *h = msgs[i];
                u64 sid = msg_sender(msgs[i]);
                Peer *p = find_peer(sid);
                if (!p && lobby_has_member(sid)) p = add_peer(sid);
                if (p) {
                    p->last_heard = GetTickCount();
                    if (h->size >= 1 && ((char *)h->data)[0] == 'B') drop_peer(p, "quit");
                }
                S.MessageRelease(msgs[i]);
            }
        }
        links_read_local();
        DWORD now = GetTickCount();
        for (int i = 0; i < MAX_PEERS; i++) {
            Peer *p = &g_peers[i];
            if (!p->sid) continue;
            if (now - p->last_ping > 1000) { p->last_ping = now; send_control(p->sid, 'K'); }
            if (now - p->last_heard > PEER_TIMEOUT_MS) drop_peer(p, "timed out");
        }
        if (g_server && WaitForSingleObject(g_server, 0) == WAIT_OBJECT_0) { printf("OOTServer stopped.\n"); break; }
        if (key_hit()) { int c = key_get(); if (c == 'q' || c == 'Q') break; }
        if (g_stop) break;
        Sleep(1);
    }
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i].sid) { send_control(g_peers[i].sid, 'B'); drop_peer(&g_peers[i], "host closed"); }
    S.LeaveLobby(g_mm, g_lobby);
    S.SetRichPresence(g_friends, "connect", "");
    S.SetRichPresence(g_friends, "status", "");
    for (int i = 0; i < 20; i++) { pump(host_cb); Sleep(10); }
    stop_server();
    return 0;
}

/* ---------------------------------------------------------------- join */

static u64 g_join_lobby, g_host;
static bool g_join_failed;

static u64 parse_connect(const char *s)
{
    const char *p = strstr(s, "+connect_lobby");
    if (!p) return 0;
    return strtoull(p + 14, NULL, 10);
}

static void join_cb(int id, void *data)
{
    switch (id) {
    case CB_LOBBY_JOIN_REQUESTED:
        if (!g_join_lobby) g_join_lobby = ((GameLobbyJoinRequested *)data)->lobby;
        break;
    case CB_RICH_PRESENCE_JOIN:
        if (!g_join_lobby) g_join_lobby = parse_connect(((GameRichPresenceJoinRequested *)data)->connect);
        break;
    case CB_LOBBY_ENTER: {
        LobbyEnter *e = data;
        if (e->lobby != g_join_lobby) break;
        if (e->response != 1) { g_join_failed = true; break; }
        g_host = S.GetLobbyOwner(g_mm, e->lobby);
        break;
    }
    case CB_SESSION_REQUEST: {
        u64 sid = S.IdentGet(data);
        if (sid == g_host) accept_session(sid);
        break;
    }
    }
}

/* Friends who host: playing AppID 480 in a lobby (the lobby tag is checked after joining). */
static int list_hosting_friends(u64 *lobbies, u64 *owners, int max)
{
    int n = 0, count = S.GetFriendCount(g_friends, 0x04 /* immediate */);
    for (int i = 0; i < count && n < max; i++) {
        u64 f = S.GetFriendByIndex(g_friends, i, 0x04);
        FriendGameInfo gi;
        memset(&gi, 0, sizeof(gi));
        if (!S.GetFriendGamePlayed(g_friends, f, &gi)) continue;
        if ((gi.game_id & 0xFFFFFF) != 480 || !gi.lobby) continue;
        lobbies[n] = gi.lobby; owners[n] = f; n++;
    }
    return n;
}

static void pick_lobby(void)
{
    u64 lobbies[32], owners[32];
    int shown = -1;
    printf("Looking for Steam friends who are hosting...\n"
           "(You can also accept a Steam invite, or type a lobby id and press Enter.)\n");
    char typed[64] = ""; int tl = 0;
    while (!g_join_lobby) {
        pump(join_cb);
        int n = list_hosting_friends(lobbies, owners, 32);
        if (n == 1) {
            printf("Found %s.\n", friend_name(owners[0]));
            g_join_lobby = lobbies[0];
            break;
        }
        if (n > 1 && n != shown) {
            printf("\nFriends hosting:\n");
            for (int i = 0; i < n; i++) printf("  %d) %s\n", i + 1, friend_name(owners[i]));
            printf("Type a number and press Enter: ");
            shown = n;
        }
        while (key_hit()) {
            int c = key_get();
            if (c == '\r') {
                printf("\n");
                u64 v = strtoull(typed, NULL, 10);
                if (v >= 1 && v <= (u64)n) g_join_lobby = lobbies[v - 1];
                else if (v > 1000) g_join_lobby = v;
                tl = 0; typed[0] = 0;
            } else if (c >= '0' && c <= '9' && tl < 60) { typed[tl++] = (char)c; typed[tl] = 0; putchar(c); }
        }
        Sleep(50);
    }
}

static SOCKET listen_local(int port, int *bound)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((u_short)port);
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 8) != 0) {
        if (s != INVALID_SOCKET) closesocket(s);
        return INVALID_SOCKET;
    }
    int len = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &len);
    *bound = ntohs(a.sin_port);
    set_nonblocking(s);
    return s;
}

static int run_join(u64 lobby)
{
    g_join_lobby = lobby;
    if (!g_join_lobby) pick_lobby();

    for (int attempt = 0; attempt < 3 && !g_host; attempt++) {
        g_join_failed = false;
        printf("Joining lobby %llu...\n", (unsigned long long)g_join_lobby);
        S.JoinLobby(g_mm, g_join_lobby);
        DWORD t0 = GetTickCount();
        while (!g_host && !g_join_failed && GetTickCount() - t0 < 15000) { pump(join_cb); Sleep(20); }
    }
    if (!g_host) fail("Could not join the lobby. Is your friend still hosting?");
    const char *tag = S.GetLobbyData(g_mm, g_join_lobby, "ootsteam");
    if (!tag || strcmp(tag, "1") != 0) fail("That lobby is not an Ocarina of Time co-op game.");
    const char *ver = S.GetLobbyData(g_mm, g_join_lobby, "ver");
    if (ver && *ver && strcmp(ver, PROTO_VERSION) != 0) fail("Your friend runs a different OOTSteamLink version. Update both.");
    printf("Host: %s\n", friend_name(g_host));
    accept_session(g_host);

    int port = 0;
    SOCKET ls = listen_local(g_local_port, &port);
    if (ls == INVALID_SOCKET) fail("Could not open the local relay port (try --local-port <number>).");
    printf("Relay listening on 127.0.0.1:%d\n", port);
    start_game("127.0.0.1", port);
    printf("Relaying 127.0.0.1:%d <-> %s over Steam. Keep this window open. Close the game to stop.\n",
           port, friend_name(g_host));
    g_cb = join_cb;

    u32 next_id = 1;
    DWORD last_heard = GetTickCount(), last_ping = 0, warned = 0;
    bool host_gone = false;
    for (;;) {
        pump(join_cb);
        for (;;) { /* new local connections */
            SOCKET c = accept(ls, NULL, NULL);
            if (c == INVALID_SOCKET) break;
            set_nonblocking(c);
            u32 id = next_id++;
            if (host_gone || !new_link(g_host, id, c)) { closesocket(c); continue; }
            if (!send_frame(g_host, 'O', id, NULL, 0)) close_link(find_link(g_host, id), false);
        }
        void *msgs[64]; int n;
        while ((n = S.ReceiveMessages(g_msgs, 0, msgs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                MsgHead *h = msgs[i];
                if (msg_sender(msgs[i]) == g_host && h->size >= 5) {
                    const unsigned char *d = h->data;
                    u32 id;
                    memcpy(&id, d + 1, 4);
                    Link *l = find_link(g_host, id);
                    last_heard = GetTickCount();
                    if (l && d[0] == 'D') { link_append(l, d + 5, h->size - 5); g_bytes_down += h->size - 5; }
                    else if (l && d[0] == 'C') { if (l->outlen) l->closing = true; else free_link(l); }
                }
                S.MessageRelease(msgs[i]);
            }
        }
        while ((n = S.ReceiveMessages(g_msgs, 1, msgs, 64)) > 0) {
            for (int i = 0; i < n; i++) {
                MsgHead *h = msgs[i];
                if (msg_sender(msgs[i]) == g_host) {
                    last_heard = GetTickCount();
                    if (h->size >= 1 && ((char *)h->data)[0] == 'B') {
                        printf("The host closed the game.\n");
                        host_gone = true;
                        close_links_of(g_host);
                    }
                }
                S.MessageRelease(msgs[i]);
            }
        }
        links_read_local();
        DWORD now = GetTickCount();
        if (now - last_ping > 1000) { last_ping = now; send_control(g_host, 'K'); }
        if (now - last_heard > PEER_TIMEOUT_MS && now - warned > PEER_TIMEOUT_MS) {
            warned = now;
            printf("No answer from the host for 20 s...\n");
            close_links_of(g_host);
        }
        if (game_exited()) break;
        if (key_hit()) { int c = key_get(); if (c == 'q' || c == 'Q' || c == 27) break; }
        if (g_stop) break;
        Sleep(1);
    }
    send_control(g_host, 'B');
    close_links_of(g_host);
    closesocket(ls);
    S.LeaveLobby(g_mm, g_join_lobby);
    for (int i = 0; i < 20; i++) { pump(join_cb); Sleep(10); }
    close_session(g_host);
    return 0;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const char *mode = NULL, *arg = NULL;
    if (argc == 2 && !strcmp(argv[1], "--launcher")) return run_launcher();
    SetConsoleTitleA("Ocarina of Time Co-op (Steam)");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--local-port") && i + 1 < argc) g_local_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--server-exe") && i + 1 < argc) snprintf(g_server_exe, sizeof(g_server_exe), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--game-exe") && i + 1 < argc) snprintf(g_game_exe, sizeof(g_game_exe), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--no-server")) g_no_server = true;
        else if (!strcmp(argv[i], "--no-game") || !strcmp(argv[i], "--no-launch")) g_no_game = true;
        else if (!strcmp(argv[i], "--quiet-errors")) g_quiet_fail = true;
        else if (!strcmp(argv[i], "+connect_lobby") && i + 1 < argc) { mode = "join"; arg = argv[++i]; }
        else if (!mode) mode = argv[i];
        else if (!arg) arg = argv[i];
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    DWORD cm;
    g_console = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &cm) != 0;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    start_launcher(); /* before steam_start, see start_launcher */
    steam_start();
    relay_config();
    printf("Ocarina of Time Co-op over Steam - signed in as %s\n\n", S.GetPersonaName(g_friends));

    if (!mode && !g_console) fail("Usage: OOTSteamLink.exe [host | join [lobby id]]");
    if (!mode) {
        printf("  1) Host a game\n  2) Join a friend's game\n\nChoose 1 or 2: ");
        for (;;) {
            int c = key_get();
            if (c == '1') { mode = "host"; break; }
            if (c == '2') { mode = "join"; break; }
            if (c == 27) return 0;
        }
        printf("%c\n\n", mode[0] == 'h' ? '1' : '2');
    }
    int r;
    if (!_stricmp(mode, "host")) r = run_host();
    else if (!_stricmp(mode, "join")) r = run_join(arg ? strtoull(arg, NULL, 10) : 0);
    else fail("Usage: OOTSteamLink.exe [host | join [lobby id]]");
    printf("Relayed %llu bytes from the game and %llu bytes to the game.\n", g_bytes_up, g_bytes_down);
    S.Shutdown();
    InterlockedExchange(&g_done, 1);
    return r;
}
