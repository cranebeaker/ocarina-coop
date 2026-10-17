#include <csignal>
#include <cstring>
#include <cstdlib>
#include "OOTServer.hpp"

static ZeldaOnline::OOTServer* g_server = nullptr;

static void onSignal(int)
{
    // lets run() return so the Story Co-op team saves are flushed
    if (g_server)
        g_server->Stop();
}

int main(int argc, char** argv)
{
    int port = 21050;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
    }
    ZeldaOnline::OOTServer server(port);
    g_server = &server;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    return server.run();
}
