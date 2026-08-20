#ifndef SERVER_HPP
#define SERVER_HPP

#include <string>
#include <vector>
#include <map>
#include <set>
#include <stdexcept>
#include <cerrno>
#include <cctype>

#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <csignal>
#include <cstring>
#include <iostream>

#include "Client.hpp"
#include "Channel.hpp"
#include "Ircmessage.hpp"

class Server
{
public:
    Server(int port, const std::string& password);
    ~Server();

    void run();

    // signal handler sets this to true to stop the loop cleanly
    static bool _stop;

private:
    // ── core state ──────────────────────────────
    int                         _port;
    std::string                 _password;
    int                         _serverFd;

    std::vector<pollfd>         _fds;       // [0] = server, [1..n] = clients
    std::map<int, Client*>      _clients;   // fd → Client*
    std::map<std::string, Channel*> _channels; // "#name" → Channel*
    std::set<int>               _closing;   // fds to close once their buffer is flushed

   
    void _createSocket();
    void _setSocketOptions();
    void _setNonBlocking(int fd);
    void _bindSocket();
    void _listenSocket();

   
    static void _signalHandler(int sig);
    void _acceptClient();
    void _readFromClient(int fd);
    void _flushWriteBuffer(int fd);
    void _disconnectClient(int fd);
    void _queueDisconnect(int fd);
    void _reapClosing();
    void sendToClient(int fd, const std::string& msg);
    void sendToAll(const std::string& msg);

    void _handleMessage(int fd, const std::string& raw);
    IRCMessage _parseMessage(const std::string& raw);

        // ── command handlers ──────────────────────
    // Person 2
    void sendWelcome(Client* client);
    void handlePASS(int fd, IRCMessage& msg);
    void handleNICK(int fd, IRCMessage& msg);
    void handleUSER(int fd, IRCMessage& msg);
    void handlePING(int fd, IRCMessage& msg);
    void handleQUIT(int fd, IRCMessage& msg);
    void handlePART(int fd, IRCMessage& msg);
    void handlePRIVMSG(int fd, IRCMessage& msg);
 
    // Person 3
    void handleJOIN(int fd, IRCMessage& msg);
    void handleKICK(int fd, IRCMessage& msg);
    void handleINVITE(int fd, IRCMessage& msg);
    void handleTOPIC(int fd, IRCMessage& msg);
    void handleMODE(int fd, IRCMessage& msg);
};


#endif