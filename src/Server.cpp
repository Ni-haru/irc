#include "Server.hpp"

#include <cstdlib>
#include <sstream>

bool Server::_stop = false;

// ═════════════════════════════════════════════════════════════════════
//  Construction / destruction
// ═════════════════════════════════════════════════════════════════════

Server::Server(int port, const std::string& password)
    : _port(port), _password(password), _serverFd(-1)
{
    _createSocket();
    _setSocketOptions();
    _setNonBlocking(_serverFd);
    _bindSocket();
    _listenSocket();

    pollfd pfd;
    pfd.fd      = _serverFd;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    _fds.push_back(pfd);

    signal(SIGINT,  Server::_signalHandler);
    signal(SIGTERM, Server::_signalHandler);
    // A client can vanish between our poll() and our send(). Writing to a
    // socket whose peer is gone raises SIGPIPE, whose default action kills
    // the process — which the subject counts as a crash. Ignoring it makes
    // send() return -1 instead, which we already handle.
    signal(SIGPIPE, SIG_IGN);

    std::cout << "Server listening on port " << _port << std::endl;
}

Server::~Server()
{
    for (std::map<int, Client*>::iterator it = _clients.begin();
         it != _clients.end(); ++it)
    {
        close(it->first);
        delete it->second;
    }
    _clients.clear();

    for (std::map<std::string, Channel*>::iterator it = _channels.begin();
         it != _channels.end(); ++it)
        delete it->second;
    _channels.clear();

    if (_serverFd != -1)
        close(_serverFd);

    std::cout << "Server shut down." << std::endl;
}

void Server::_signalHandler(int sig)
{
    (void)sig;
    Server::_stop = true;
}

// ═════════════════════════════════════════════════════════════════════
//  The event loop — the one and only poll() in the project
// ═════════════════════════════════════════════════════════════════════

void Server::run()
{
    while (!Server::_stop)
    {
        // &_fds[0] rather than _fds.data(): data() is C++11. _fds always
        // holds at least the listening socket, so indexing 0 is safe.
        int ready = poll(&_fds[0], _fds.size(), -1);

        if (ready == -1)
        {
            // poll() returning -1 here is either our own SIGINT interrupting
            // it, or a real failure. We do not inspect errno to retry an I/O
            // operation — the subject forbids that.
            if (Server::_stop)
                break;
            std::cerr << "poll() error" << std::endl;
            break;
        }

        if (_fds[0].revents & POLLIN)
            _acceptClient();

        for (size_t i = 1; i < _fds.size(); i++)
        {
            int fd = _fds[i].fd;

            if (_fds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
            {
                _disconnectClient(fd);
                i--;                    // element i was erased, re-check it
                continue;
            }
            if (_fds[i].revents & POLLIN)
            {
                _readFromClient(fd);
                if (!_clients.count(fd))
                {
                    i--;
                    continue;
                }
            }
            if (_fds[i].revents & POLLOUT)
            {
                _flushWriteBuffer(fd);
                if (!_clients.count(fd))
                {
                    i--;
                    continue;
                }
            }
        }

        _reapClosing();

        // Arm/disarm POLLOUT once per iteration. We never call send() unless
        // poll() has just told us the socket is writable, so everything the
        // handlers produced during this iteration goes out on the next one.
        for (size_t i = 1; i < _fds.size(); i++)
        {
            std::map<int, Client*>::iterator it = _clients.find(_fds[i].fd);
            if (it != _clients.end())
            {
                if (it->second->getWriteBuffer().empty())
                    _fds[i].events &= ~POLLOUT;
                else
                    _fds[i].events |= POLLOUT;
            }
        }
    }
}

void Server::_acceptClient()
{
    struct sockaddr_in clientAddr;
    socklen_t          addrLen = sizeof(clientAddr);

    int clientFd = accept(_serverFd, (struct sockaddr*)&clientAddr, &addrLen);
    if (clientFd == -1)
    {
        // No errno-driven retry: poll() said the listening socket was ready,
        // and if accept() still failed we simply skip this iteration.
        std::cerr << "accept() failed" << std::endl;
        return;
    }
    _setNonBlocking(clientFd);

    pollfd pfd;
    pfd.fd      = clientFd;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    _fds.push_back(pfd);

    Client* client = new Client(clientFd);
    // The host part of a client's prefix (nick!user@host) must never be
    // empty: clients parse the prefix to know who sent a message, and an
    // empty host makes "alice!bob@" which several clients reject outright.
    client->setHostname(inet_ntoa(clientAddr.sin_addr));
    _clients[clientFd] = client;

    std::cout << "New client connected: fd=" << clientFd << std::endl;
}

void Server::_readFromClient(int fd)
{
    // One recv() per POLLIN, never a loop: the subject requires poll() before
    // every read. 4096 instead of 512 only changes how much of an already
    // available burst we drain per iteration — it keeps a flooding client
    // from starving the other connections for many loop turns.
    char    buf[4096];
    ssize_t bytes = recv(fd, buf, sizeof(buf), 0);

    if (bytes <= 0)
    {
        // 0 = orderly shutdown, -1 = error. Either way the connection is
        // over; we do not look at errno to decide to read again.
        _disconnectClient(fd);
        return;
    }

    std::map<int, Client*>::iterator it = _clients.find(fd);
    if (it == _clients.end())
        return;
    it->second->appendToBuffer(std::string(buf, bytes));

    // TCP is a byte stream, so one recv() may carry half a command, one
    // command, or five. We only execute the complete lines sitting in the
    // buffer and leave any partial tail there for the next POLLIN.
    std::string msg;
    while (_clients.count(fd) && !_closing.count(fd)
           && _clients[fd]->getNextMessage(msg))
        _handleMessage(fd, msg);
}

void Server::_flushWriteBuffer(int fd)
{
    if (!_clients.count(fd))
        return;
    Client* client = _clients[fd];
    const std::string& buf = client->getWriteBuffer();

    if (buf.empty())
        return;

    ssize_t sent = send(fd, buf.c_str(), buf.size(), 0);
    if (sent > 0)
    {
        // A partial write is normal: the kernel accepted what fitted in the
        // socket buffer. We drop exactly that many bytes and the rest waits
        // for the next POLLOUT.
        client->clearWriteBuffer(static_cast<int>(sent));
        if (client->getWriteBuffer().empty() && _closing.count(fd))
            _disconnectClient(fd);
    }
    else
    {
        // poll() had just reported the socket writable, so a failure here is
        // a dead peer, not a would-block situation.
        _disconnectClient(fd);
    }
}

// Ask for a disconnect that must NOT lose the bytes still queued for the
// client (the 464 password error, the ERROR line after QUIT...). If nothing
// is pending we close right away; otherwise we remember the fd and close it
// in _reapClosing() once _flushWriteBuffer() has drained the buffer.
void Server::_queueDisconnect(int fd)
{
    if (!_clients.count(fd))
        return;
    if (_clients[fd]->getWriteBuffer().empty())
    {
        _disconnectClient(fd);
        return;
    }
    _closing.insert(fd);
}

void Server::_reapClosing()
{
    std::set<int>::iterator it = _closing.begin();
    while (it != _closing.end())
    {
        int fd = *it;
        ++it;
        if (!_clients.count(fd))
            _closing.erase(fd);
        else if (_clients[fd]->getWriteBuffer().empty())
            _disconnectClient(fd);
    }
}

void Server::_disconnectClient(int fd)
{
    _closing.erase(fd);

    std::map<int, Client*>::iterator cit = _clients.find(fd);
    if (cit != _clients.end())
    {
        Client* client = cit->second;

        // Tell the channels the user was in, before the object dies. Every
        // Channel holds raw Client* — if we deleted the Client first, those
        // pointers would dangle and the next broadcast would segfault.
        std::string quitMsg;
        if (client->isFullyRegistered())
            quitMsg = ":" + client->getPrefix() + " QUIT :Connection closed\r\n";

        std::map<std::string, Channel*>::iterator ch = _channels.begin();
        while (ch != _channels.end())
        {
            Channel* channel = ch->second;
            bool wasMember = channel->hasClient(client);

            channel->removeClient(client);
            channel->removeOperator(client);
            channel->removeInvited(client);

            if (wasMember && !quitMsg.empty())
                channel->broadcast(quitMsg, NULL);

            if (channel->empty())
            {
                delete channel;
                _channels.erase(ch++);   // post-increment: the iterator is
            }                            // advanced before erase invalidates it
            else
                ++ch;
        }

        delete client;
        _clients.erase(cit);
    }

    for (size_t i = 0; i < _fds.size(); i++)
    {
        if (_fds[i].fd == fd)
        {
            _fds.erase(_fds.begin() + i);
            break;
        }
    }
    close(fd);
    std::cout << "Client disconnected: fd=" << fd << std::endl;
}

// ═════════════════════════════════════════════════════════════════════
//  Socket setup
// ═════════════════════════════════════════════════════════════════════

void Server::_createSocket()
{
    _serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (_serverFd == -1)
        throw std::runtime_error("socket() failed");
}

void Server::_setSocketOptions()
{
    int opt = 1;
    if (setsockopt(_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        close(_serverFd);
        throw std::runtime_error("setsockopt() failed");
    }
}

void Server::_setNonBlocking(int fd)
{
    // The only form of fcntl() the subject allows.
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1)
    {
        close(fd);
        throw std::runtime_error("fcntl() failed");
    }
}

void Server::_bindSocket()
{
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(_port);
    addr.sin_addr.s_addr = INADDR_ANY;   // listen on every interface

    if (bind(_serverFd, (struct sockaddr*)&addr, sizeof(addr)) == -1)
    {
        close(_serverFd);
        throw std::runtime_error("bind() failed: port may already be in use");
    }
}

void Server::_listenSocket()
{
    if (listen(_serverFd, 10) == -1)
    {
        close(_serverFd);
        throw std::runtime_error("listen() failed");
    }
}

// ═════════════════════════════════════════════════════════════════════
//  Parsing and small helpers
// ═════════════════════════════════════════════════════════════════════

IRCMessage Server::_parseMessage(const std::string& raw)
{
    IRCMessage msg;
    std::string line = raw;
    size_t pos = 0;

    if (!line.empty() && line[0] == ':')
    {
        pos = line.find(' ');
        if (pos == std::string::npos)
            return msg;
        msg.prefix = line.substr(1, pos - 1);
        line = line.substr(pos + 1);
    }

    // A client may send extra spaces; skip them so " PING" still parses.
    while (!line.empty() && line[0] == ' ')
        line.erase(0, 1);

    pos = line.find(' ');
    if (pos == std::string::npos)
    {
        msg.command = line;
        return msg;
    }
    msg.command = line.substr(0, pos);
    line = line.substr(pos + 1);

    while (!line.empty())
    {
        if (line[0] == ' ')
        {
            line.erase(0, 1);
            continue;
        }
        if (line[0] == ':')
        {
            // The trailing parameter: everything after the colon is one
            // single argument, spaces included.
            msg.params.push_back(line.substr(1));
            break;
        }
        pos = line.find(' ');
        if (pos == std::string::npos)
        {
            msg.params.push_back(line);
            break;
        }
        msg.params.push_back(line.substr(0, pos));
        line = line.substr(pos + 1);
    }

    return msg;
}

std::vector<std::string> Server::_split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string current;
    for (std::size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == sep)
        {
            out.push_back(current);
            current.clear();
        }
        else
            current += s[i];
    }
    out.push_back(current);
    return out;
}

Client* Server::_findClientByNick(const std::string& nick)
{
    std::string key = IRC::toLower(nick);
    for (std::map<int, Client*>::iterator it = _clients.begin();
         it != _clients.end(); ++it)
    {
        if (IRC::toLower(it->second->getNickname()) == key)
            return it->second;
    }
    return NULL;
}

Channel* Server::_findChannel(const std::string& name)
{
    std::map<std::string, Channel*>::iterator it =
        _channels.find(IRC::toLower(name));
    if (it == _channels.end())
        return NULL;
    return it->second;
}

void Server::_removeChannelIfEmpty(Channel* channel)
{
    if (!channel || !channel->empty())
        return;
    std::map<std::string, Channel*>::iterator it =
        _channels.find(IRC::toLower(channel->getName()));
    if (it != _channels.end())
    {
        delete it->second;
        _channels.erase(it);
    }
}

bool Server::_isValidChannelName(const std::string& name)
{
    if (name.size() < 2 || name.size() > 50)
        return false;
    if (name[0] != '#' && name[0] != '&')
        return false;
    for (std::size_t i = 0; i < name.size(); i++)
    {
        if (name[i] == ' ' || name[i] == ',' || name[i] == 7)
            return false;
    }
    return true;
}

std::string Server::_modeString(Channel* channel, bool withKeyAndLimit)
{
    std::string modes = "+";
    std::string args;

    if (channel->isInviteOnly())      modes += "i";
    if (channel->isTopicRestricted()) modes += "t";
    if (channel->hasKey())
    {
        modes += "k";
        if (withKeyAndLimit)
            args += " *";               // never leak the key in a reply
    }
    if (channel->hasLimit())
    {
        modes += "l";
        if (withKeyAndLimit)
        {
            std::ostringstream ss;
            ss << " " << channel->getUserLimit();
            args += ss.str();
        }
    }
    return modes + args;
}

void Server::_sendNames(int fd, Channel* channel)
{
    Client* client = _clients[fd];
    const std::vector<Client*>& members = channel->getClients();

    std::string names;
    for (size_t i = 0; i < members.size(); i++)
    {
        if (i)
            names += " ";
        if (channel->isOperator(members[i]))
            names += "@";
        names += members[i]->getNickname();
    }

    sendToClient(fd, IRC::makeReply(IRC::RPL_NAMREPLY,
        client->getNickname() + " = " + channel->getName(), names));
    sendToClient(fd, IRC::makeReply(IRC::RPL_ENDOFNAMES,
        client->getNickname() + " " + channel->getName(), "End of /NAMES list"));
}

// ═════════════════════════════════════════════════════════════════════
//  Output
// ═════════════════════════════════════════════════════════════════════

// This only *queues*. The actual send() happens in _flushWriteBuffer(),
// which runs only after poll() reported POLLOUT. That separation is what
// keeps the project inside the "one poll(), no send without poll()" rule.
void Server::sendToClient(int fd, const std::string& msg)
{
    if (!_clients.count(fd))
        return;
    _clients[fd]->queueMessage(msg);
}

void Server::sendToAll(const std::string& msg)
{
    for (std::map<int, Client*>::iterator it = _clients.begin();
         it != _clients.end(); ++it)
        sendToClient(it->first, msg);
}

// ═════════════════════════════════════════════════════════════════════
//  Dispatch
// ═════════════════════════════════════════════════════════════════════

void Server::_handleMessage(int fd, const std::string& raw)
{
    if (raw.empty())
        return;

    IRCMessage msg = _parseMessage(raw);
    if (msg.command.empty())
        return;

    // IRC command verbs are case-insensitive.
    std::string cmd = IRC::toUpper(msg.command);
    msg.command = cmd;

    if (!_clients.count(fd))
        return;
    Client* client = _clients[fd];

    // NOTE: there is deliberately no per-command logging here.
    // std::cout is a blocking file descriptor and we never poll() it. If the
    // process is launched with its stdout on a pipe that nobody drains (a
    // test harness, `./ircserv 6667 pw | tee log`, ...), the 64 KB pipe
    // buffer fills after a few thousand lines and the next write() blocks
    // the whole server inside the event loop — no poll(), no reads, no
    // accepts. A flood of messages is exactly what fills it.
    try
    {
        // ── allowed before registration completes ──
        if      (cmd == "CAP")  { handleCAP(fd, msg);  return; }
        else if (cmd == "PASS") { handlePASS(fd, msg); return; }
        else if (cmd == "NICK") { handleNICK(fd, msg); return; }
        else if (cmd == "USER") { handleUSER(fd, msg); return; }
        else if (cmd == "QUIT") { handleQUIT(fd, msg); return; }
        else if (cmd == "PING") { handlePING(fd, msg); return; }
        else if (cmd == "PONG") { handlePONG(fd, msg); return; }

        // ── everything below needs a registered user ──
        if (!client->isFullyRegistered())
        {
            // RFC: a NOTICE must never trigger an automatic reply,
            // otherwise two servers could ping-pong error messages forever.
            if (cmd != "NOTICE")
            {
                std::string nick = client->getNickname().empty()
                                 ? "*" : client->getNickname();
                sendToClient(fd, IRC::makeReply(IRC::ERR_NOTREGISTERED,
                    nick, "You have not registered"));
            }
            return;
        }

        if      (cmd == "PRIVMSG") handlePRIVMSG(fd, msg, false);
        else if (cmd == "NOTICE")  handlePRIVMSG(fd, msg, true);
        else if (cmd == "PART")    handlePART(fd, msg);
        else if (cmd == "JOIN")    handleJOIN(fd, msg);
        else if (cmd == "KICK")    handleKICK(fd, msg);
        else if (cmd == "INVITE")  handleINVITE(fd, msg);
        else if (cmd == "TOPIC")   handleTOPIC(fd, msg);
        else if (cmd == "MODE")    handleMODE(fd, msg);
        else if (cmd == "WHO")     handleWHO(fd, msg);
        else if (cmd == "WHOIS")   handleWHOIS(fd, msg);
        else if (cmd == "NAMES")   handleNAMES(fd, msg);
        else if (cmd == "LIST")    handleLIST(fd, msg);
        else
        {
            sendToClient(fd, IRC::makeReply(IRC::ERR_UNKNOWNCOMMAND,
                client->getNickname() + " " + cmd, "Unknown command"));
        }
    }
    catch (const std::exception& e)
    {
        // A throw inside a handler must never take the whole server down.
        std::cerr << "Error handling message on fd " << fd
                  << ": " << e.what() << std::endl;
    }
}

// ═════════════════════════════════════════════════════════════════════
//  Registration: PASS / NICK / USER
// ═════════════════════════════════════════════════════════════════════

void Server::handlePASS(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (client->isFullyRegistered())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_ALREADYREGISTRED,
            client->getNickname(), "You may not reregister"));
        return;
    }
    if (msg.params.empty() || msg.params[0].empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
            "* PASS", "Not enough parameters"));
        return;
    }
    if (msg.params[0] != _password)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_PASSWDMISMATCH,
            "*", "Password incorrect"));
        sendToClient(fd, "ERROR :Closing link (Bad password)\r\n");
        // Queue, don't close: the two lines above still have to reach the
        // client, and they are only written on the next POLLOUT.
        _queueDisconnect(fd);
        return;
    }
    client->setPassAccepted(true);
}

void Server::sendWelcome(Client* client)
{
    int fd = client->getFd();
    const std::string& nick = client->getNickname();

    sendToClient(fd, IRC::makeReply(IRC::RPL_WELCOME, nick,
        "Welcome to the IRC Network " + client->getPrefix()));
    sendToClient(fd, IRC::makeReply(IRC::RPL_YOURHOST, nick,
        "Your host is ircserv running version 1.0"));
    sendToClient(fd, IRC::makeReply(IRC::RPL_CREATED, nick,
        "This server was created today"));
    sendToClient(fd, IRC::makeRawReply(IRC::RPL_MYINFO,
        nick + " ircserv 1.0 o itkol"));
}

void Server::handleNICK(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (!client->isPassAccepted())
    {
        std::string nick = client->getNickname().empty()
                         ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTREGISTERED,
            nick, "You have not registered"));
        return;
    }
    if (msg.params.empty() || msg.params[0].empty())
    {
        std::string nick = client->getNickname().empty()
                         ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_NONICKNAMEGIVEN,
            nick, "No nickname given"));
        return;
    }

    std::string newNick = msg.params[0];
    std::string current  = client->getNickname().empty()
                         ? "*" : client->getNickname();

    bool invalidChar = false;
    const std::string allowed = "[]{}\\|-_^";
    for (std::size_t i = 0; i < newNick.size(); i++)
    {
        if (!std::isalnum(static_cast<unsigned char>(newNick[i]))
            && allowed.find(newNick[i]) == std::string::npos)
        {
            invalidChar = true;
            break;
        }
    }
    if (invalidChar || newNick.size() > 9
        || std::isdigit(static_cast<unsigned char>(newNick[0])))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_ERRONEUSNICK,
            current + " " + newNick, "Erroneous nickname"));
        return;
    }

    // Collision test uses the RFC casemapping: "Bob" and "bob" are the same
    // nickname, so the second one must be refused.
    Client* holder = _findClientByNick(newNick);
    if (holder && holder != client)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NICKNAMEINUSE,
            current + " " + newNick, "Nickname is already in use"));
        return;
    }

    // Whether this NICK completes the registration or renames an already
    // registered user has to be decided BEFORE we store the new nickname —
    // otherwise a client that sent USER first would look "already
    // registered" the instant we set the nick and would never get its 001.
    bool wasRegistered = client->isFullyRegistered();
    std::string oldPrefix = client->getPrefix();

    client->setNickname(newNick);
    client->setNickSet(true);

    if (!wasRegistered)
    {
        if (client->isFullyRegistered())
            sendWelcome(client);
        return;
    }

    // Rename: the change is echoed to the user and to every channel they
    // share, using the OLD prefix as the source of the message.
    std::string nickMsg = ":" + oldPrefix + " NICK :" + newNick + "\r\n";
    sendToClient(fd, nickMsg);
    for (std::map<std::string, Channel*>::iterator it = _channels.begin();
         it != _channels.end(); ++it)
    {
        if (it->second->hasClient(client))
            it->second->broadcast(nickMsg, client);
    }
}

void Server::handleUSER(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (!client->isPassAccepted())
    {
        std::string nick = client->getNickname().empty()
                         ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTREGISTERED,
            nick, "You have not registered"));
        return;
    }
    if (client->isUserSet())
    {
        std::string nick = client->getNickname().empty()
                         ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_ALREADYREGISTRED,
            nick, "You may not reregister"));
        return;
    }
    if (msg.params.size() < 4)
    {
        std::string nick = client->getNickname().empty()
                         ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
            nick + " USER", "Not enough parameters"));
        return;
    }

    client->setUsername(msg.params[0]);
    client->setRealname(msg.params[3]);
    client->setUserSet(true);

    if (client->isFullyRegistered())
        sendWelcome(client);
}

// ═════════════════════════════════════════════════════════════════════
//  Connection keep-alive and teardown
// ═════════════════════════════════════════════════════════════════════

void Server::handlePING(int fd, IRCMessage& msg)
{
    if (msg.params.empty() || msg.params[0].empty())
    {
        std::string nick = _clients[fd]->getNickname();
        if (nick.empty())
            nick = "*";
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOORIGIN,
            nick, "No origin specified"));
        return;
    }
    // The token the client sent must come back untouched — that is how the
    // client matches the reply to its own ping and measures the lag.
    sendToClient(fd, ":ircserv PONG ircserv :" + msg.params[0] + "\r\n");
}

void Server::handlePONG(int fd, IRCMessage& msg)
{
    // We never ping clients ourselves, so an incoming PONG is simply
    // absorbed. Answering it with 421 would confuse real clients.
    (void)fd;
    (void)msg;
}

void Server::handleQUIT(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (client->isFullyRegistered())
    {
        std::string reason = msg.params.empty() ? "Client Quit" : msg.params[0];
        std::string quitMsg = ":" + client->getPrefix() + " QUIT :" + reason + "\r\n";

        for (std::map<std::string, Channel*>::iterator it = _channels.begin();
             it != _channels.end(); ++it)
        {
            if (it->second->hasClient(client))
                it->second->broadcast(quitMsg, client);
        }
    }
    sendToClient(fd, "ERROR :Closing connection\r\n");
    _queueDisconnect(fd);
}

// ═════════════════════════════════════════════════════════════════════
//  Messaging
// ═════════════════════════════════════════════════════════════════════

void Server::handlePRIVMSG(int fd, IRCMessage& msg, bool isNotice)
{
    Client* client = _clients[fd];
    std::string cmd = isNotice ? "NOTICE" : "PRIVMSG";

    if (msg.params.empty())
    {
        if (!isNotice)
            sendToClient(fd, IRC::makeReply(IRC::ERR_NORECIPIENT,
                client->getNickname(), "No recipient given (" + cmd + ")"));
        return;
    }
    if (msg.params.size() < 2 || msg.params[1].empty())
    {
        if (!isNotice)
            sendToClient(fd, IRC::makeReply(IRC::ERR_NOTEXTTOSEND,
                client->getNickname(), "No text to send"));
        return;
    }

    std::vector<std::string> targets = _split(msg.params[0], ',');
    for (size_t i = 0; i < targets.size(); i++)
    {
        const std::string& target = targets[i];
        if (target.empty())
            continue;

        if (target[0] == '#' || target[0] == '&')
        {
            Channel* channel = _findChannel(target);
            if (!channel)
            {
                if (!isNotice)
                    sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL,
                        client->getNickname() + " " + target, "No such channel"));
                continue;
            }
            if (!channel->hasClient(client))
            {
                if (!isNotice)
                    sendToClient(fd, IRC::makeReply(IRC::ERR_CANNOTSENDTOCHAN,
                        client->getNickname() + " " + target,
                        "Cannot send to channel"));
                continue;
            }
            std::string full = ":" + client->getPrefix() + " " + cmd + " "
                             + channel->getName() + " :" + msg.params[1] + "\r\n";
            // sender excluded: their own client already displayed the line
            channel->broadcast(full, client);
        }
        else
        {
            Client* dest = _findClientByNick(target);
            if (!dest)
            {
                if (!isNotice)
                    sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHNICK,
                        client->getNickname() + " " + target,
                        "No such nick/channel"));
                continue;
            }
            std::string full = ":" + client->getPrefix() + " " + cmd + " "
                             + dest->getNickname() + " :" + msg.params[1] + "\r\n";
            sendToClient(dest->getFd(), full);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════
//  Channels
// ═════════════════════════════════════════════════════════════════════

void Server::handleJOIN(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (msg.params.empty() || msg.params[0].empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
            client->getNickname() + " JOIN", "Not enough parameters"));
        return;
    }

    // "JOIN #a,#b,#c key1,key2" — the Nth key belongs to the Nth channel.
    std::vector<std::string> names = _split(msg.params[0], ',');
    std::vector<std::string> keys;
    if (msg.params.size() > 1)
        keys = _split(msg.params[1], ',');

    for (size_t i = 0; i < names.size(); i++)
    {
        if (names[i].empty())
            continue;
        std::string key = (i < keys.size()) ? keys[i] : "";
        _joinChannel(fd, names[i], key);
        if (!_clients.count(fd))
            return;
    }
}

void Server::_joinChannel(int fd, const std::string& name, const std::string& key)
{
    Client* client = _clients[fd];

    if (!_isValidChannelName(name))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL,
            client->getNickname() + " " + name, "No such channel"));
        return;
    }

    Channel* channel = _findChannel(name);
    bool created = false;
    if (!channel)
    {
        channel = new Channel(name);
        _channels[IRC::toLower(name)] = channel;
        created = true;
    }

    if (channel->hasClient(client))
        return;                       // already there: JOIN is a no-op

    if (!created)
    {
        if (channel->isInviteOnly() && !channel->isInvited(client))
        {
            sendToClient(fd, IRC::makeReply(IRC::ERR_INVITEONLYCHAN,
                client->getNickname() + " " + channel->getName(),
                "Cannot join channel (+i)"));
            return;
        }
        if (channel->hasKey() && !channel->checkKey(key))
        {
            sendToClient(fd, IRC::makeReply(IRC::ERR_BADCHANNELKEY,
                client->getNickname() + " " + channel->getName(),
                "Cannot join channel (+k)"));
            return;
        }
        if (channel->hasLimit() && channel->isFull())
        {
            sendToClient(fd, IRC::makeReply(IRC::ERR_CHANNELISFULL,
                client->getNickname() + " " + channel->getName(),
                "Cannot join channel (+l)"));
            return;
        }
    }

    channel->addClient(client);        // first member becomes operator
    channel->removeInvited(client);    // an invite is single-use

    // The JOIN echo goes to everybody INCLUDING the new member: that is how
    // their own client learns the join succeeded and opens the window.
    std::string joinMsg = ":" + client->getPrefix() + " JOIN "
                        + channel->getName() + "\r\n";
    channel->broadcast(joinMsg, NULL);

    if (channel->getTopic().empty())
        sendToClient(fd, IRC::makeReply(IRC::RPL_NOTOPIC,
            client->getNickname() + " " + channel->getName(), "No topic is set"));
    else
        sendToClient(fd, IRC::makeReply(IRC::RPL_TOPIC,
            client->getNickname() + " " + channel->getName(), channel->getTopic()));

    _sendNames(fd, channel);
}

void Server::handlePART(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (msg.params.empty() || msg.params[0].empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
            client->getNickname() + " PART", "Not enough parameters"));
        return;
    }

    std::vector<std::string> names = _split(msg.params[0], ',');
    std::string reason = (msg.params.size() > 1) ? msg.params[1] : "";

    for (size_t i = 0; i < names.size(); i++)
    {
        if (names[i].empty())
            continue;

        Channel* channel = _findChannel(names[i]);
        if (!channel)
        {
            sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL,
                client->getNickname() + " " + names[i], "No such channel"));
            continue;
        }
        if (!channel->hasClient(client))
        {
            sendToClient(fd, IRC::makeReply(IRC::ERR_NOTONCHANNEL,
                client->getNickname() + " " + channel->getName(),
                "You're not on that channel"));
            continue;
        }

        std::string partMsg = ":" + client->getPrefix() + " PART "
                            + channel->getName();
        if (!reason.empty())
            partMsg += " :" + reason;
        partMsg += "\r\n";

        // Broadcast first (NULL = nobody excluded) so the leaving user also
        // sees the confirmation, then actually remove them.
        channel->broadcast(partMsg, NULL);
        channel->removeClient(client);
        channel->removeOperator(client);
        _removeChannelIfEmpty(channel);
    }
}

void Server::handleKICK(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (msg.params.size() < 2)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
            client->getNickname() + " KICK", "Not enough parameters"));
        return;
    }

    Channel* channel = _findChannel(msg.params[0]);
    if (!channel)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL,
            client->getNickname() + " " + msg.params[0], "No such channel"));
        return;
    }
    if (!channel->hasClient(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTONCHANNEL,
            client->getNickname() + " " + channel->getName(),
            "You're not on that channel"));
        return;
    }
    if (!channel->isOperator(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_CHANOPRIVSNEEDED,
            client->getNickname() + " " + channel->getName(),
            "You're not channel operator"));
        return;
    }

    std::vector<std::string> victims = _split(msg.params[1], ',');
    for (size_t i = 0; i < victims.size(); i++)
    {
        if (victims[i].empty())
            continue;

        Client* target = _findClientByNick(victims[i]);
        if (!target)
        {
            sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHNICK,
                client->getNickname() + " " + victims[i],
                "No such nick/channel"));
            continue;
        }
        if (!channel->hasClient(target))
        {
            sendToClient(fd, IRC::makeReply(IRC::ERR_USERNOTINCHANNEL,
                client->getNickname() + " " + target->getNickname()
                + " " + channel->getName(), "They aren't on that channel"));
            continue;
        }

        std::string kickMsg = ":" + client->getPrefix() + " KICK "
                            + channel->getName() + " " + target->getNickname()
                            + " :" + (msg.params.size() > 2 ? msg.params[2]
                                                            : client->getNickname())
                            + "\r\n";

        // Broadcast before removing so the kicked user is told why.
        channel->broadcast(kickMsg, NULL);
        channel->removeClient(target);
        channel->removeOperator(target);
    }
    _removeChannelIfEmpty(channel);
}

void Server::handleINVITE(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (msg.params.size() < 2)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
            client->getNickname() + " INVITE", "Not enough parameters"));
        return;
    }

    Channel* channel = _findChannel(msg.params[1]);
    if (!channel)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL,
            client->getNickname() + " " + msg.params[1], "No such channel"));
        return;
    }
    if (!channel->hasClient(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTONCHANNEL,
            client->getNickname() + " " + channel->getName(),
            "You're not on that channel"));
        return;
    }
    if (channel->isInviteOnly() && !channel->isOperator(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_CHANOPRIVSNEEDED,
            client->getNickname() + " " + channel->getName(),
            "You're not channel operator"));
        return;
    }

    Client* target = _findClientByNick(msg.params[0]);
    if (!target)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHNICK,
            client->getNickname() + " " + msg.params[0], "No such nick/channel"));
        return;
    }
    if (channel->hasClient(target))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_USERONCHANNEL,
            client->getNickname() + " " + target->getNickname()
            + " " + channel->getName(), "is already on channel"));
        return;
    }

    channel->addInvited(target);

    sendToClient(fd, IRC::makeRawReply(IRC::RPL_INVITING,
        client->getNickname() + " " + target->getNickname()
        + " " + channel->getName()));
    sendToClient(target->getFd(), ":" + client->getPrefix() + " INVITE "
        + target->getNickname() + " :" + channel->getName() + "\r\n");
}

void Server::handleTOPIC(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (msg.params.empty() || msg.params[0].empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
            client->getNickname() + " TOPIC", "Not enough parameters"));
        return;
    }

    Channel* channel = _findChannel(msg.params[0]);
    if (!channel)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL,
            client->getNickname() + " " + msg.params[0], "No such channel"));
        return;
    }
    if (!channel->hasClient(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTONCHANNEL,
            client->getNickname() + " " + channel->getName(),
            "You're not on that channel"));
        return;
    }

    // One parameter = "show me the topic", two = "set the topic".
    if (msg.params.size() == 1)
    {
        if (channel->getTopic().empty())
            sendToClient(fd, IRC::makeReply(IRC::RPL_NOTOPIC,
                client->getNickname() + " " + channel->getName(),
                "No topic is set"));
        else
            sendToClient(fd, IRC::makeReply(IRC::RPL_TOPIC,
                client->getNickname() + " " + channel->getName(),
                channel->getTopic()));
        return;
    }

    if (channel->isTopicRestricted() && !channel->isOperator(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_CHANOPRIVSNEEDED,
            client->getNickname() + " " + channel->getName(),
            "You're not channel operator"));
        return;
    }

    channel->setTopic(msg.params[1]);
    channel->broadcast(":" + client->getPrefix() + " TOPIC "
        + channel->getName() + " :" + msg.params[1] + "\r\n", NULL);
}

// ═════════════════════════════════════════════════════════════════════
//  MODE
// ═════════════════════════════════════════════════════════════════════

void Server::handleMODE(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (msg.params.empty() || msg.params[0].empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
            client->getNickname() + " MODE", "Not enough parameters"));
        return;
    }

    // MODE has two completely different meanings depending on the target.
    // Clients send "MODE <own nick>" right after connecting, so we must not
    // answer that with "No such channel".
    if (msg.params[0][0] != '#' && msg.params[0][0] != '&')
    {
        handleUserMode(fd, msg);
        return;
    }

    Channel* channel = _findChannel(msg.params[0]);
    if (!channel)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL,
            client->getNickname() + " " + msg.params[0], "No such channel"));
        return;
    }

    // A bare "MODE #chan" is a query: report the current modes.
    if (msg.params.size() == 1)
    {
        sendToClient(fd, IRC::makeRawReply(IRC::RPL_CHANNELMODEIS,
            client->getNickname() + " " + channel->getName() + " "
            + _modeString(channel, channel->hasClient(client))));
        return;
    }

    if (!channel->hasClient(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTONCHANNEL,
            client->getNickname() + " " + channel->getName(),
            "You're not on that channel"));
        return;
    }
    if (!channel->isOperator(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_CHANOPRIVSNEEDED,
            client->getNickname() + " " + channel->getName(),
            "You're not channel operator"));
        return;
    }

    const std::string& modes = msg.params[1];
    char   sign     = '+';
    size_t argIndex = 2;

    // We rebuild the list of changes that were actually applied, so the
    // broadcast reflects reality instead of echoing the raw request.
    std::string appliedModes;
    std::string appliedArgs;
    char        appliedSign = 0;

    for (size_t i = 0; i < modes.size(); i++)
    {
        char c = modes[i];
        if (c == '+' || c == '-')
        {
            sign = c;
            continue;
        }

        if (c == 'i' || c == 't')
        {
            if (c == 'i') channel->setInviteOnly(sign == '+');
            else          channel->setTopicRestricted(sign == '+');
            if (appliedSign != sign) { appliedModes += sign; appliedSign = sign; }
            appliedModes += c;
        }
        else if (c == 'k')
        {
            if (sign == '+')
            {
                if (argIndex >= msg.params.size())
                {
                    sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
                        client->getNickname() + " MODE", "Not enough parameters"));
                    return;
                }
                std::string key = msg.params[argIndex++];
                channel->setKey(key);
                appliedArgs += " " + key;
            }
            else
                channel->setKey("");
            if (appliedSign != sign) { appliedModes += sign; appliedSign = sign; }
            appliedModes += c;
        }
        else if (c == 'l')
        {
            if (sign == '+')
            {
                if (argIndex >= msg.params.size())
                {
                    sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
                        client->getNickname() + " MODE", "Not enough parameters"));
                    return;
                }
                std::string raw = msg.params[argIndex++];
                int limit = std::atoi(raw.c_str());
                if (limit <= 0)
                    continue;             // silently ignore a nonsense limit
                channel->setUserLimit(limit);
                appliedArgs += " " + raw;
            }
            else
                channel->setUserLimit(-1);
            if (appliedSign != sign) { appliedModes += sign; appliedSign = sign; }
            appliedModes += c;
        }
        else if (c == 'o')
        {
            if (argIndex >= msg.params.size())
            {
                sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
                    client->getNickname() + " MODE", "Not enough parameters"));
                return;
            }
            std::string nick   = msg.params[argIndex++];
            Client*     target = _findClientByNick(nick);

            if (!target || !channel->hasClient(target))
            {
                sendToClient(fd, IRC::makeReply(IRC::ERR_USERNOTINCHANNEL,
                    client->getNickname() + " " + nick + " " + channel->getName(),
                    "They aren't on that channel"));
                continue;
            }
            if (sign == '+') channel->addOperator(target);
            else             channel->removeOperator(target);

            if (appliedSign != sign) { appliedModes += sign; appliedSign = sign; }
            appliedModes += c;
            appliedArgs  += " " + target->getNickname();
        }
        else
        {
            sendToClient(fd, IRC::makeReply(IRC::ERR_UNKNOWNMODE,
                client->getNickname() + " " + std::string(1, c),
                "is unknown mode char to me"));
        }
    }

    if (appliedModes.empty())
        return;

    channel->broadcast(":" + client->getPrefix() + " MODE "
        + channel->getName() + " " + appliedModes + appliedArgs + "\r\n", NULL);
}

void Server::handleUserMode(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    // A user may only look at their own modes.
    if (IRC::toLower(msg.params[0]) != IRC::toLower(client->getNickname()))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_USERSDONTMATCH,
            client->getNickname(), "Cannot change mode for other users"));
        return;
    }
    // We implement no user modes, so any change request is answered with the
    // (unchanged) current state rather than an error.
    sendToClient(fd, IRC::makeRawReply(IRC::RPL_UMODEIS,
        client->getNickname() + " +i"));
}

// ═════════════════════════════════════════════════════════════════════
//  Commands that IRC clients send by themselves
// ═════════════════════════════════════════════════════════════════════

void Server::handleCAP(int fd, IRCMessage& msg)
{
    // IRCv3 capability negotiation. We support no capabilities, but the
    // handshake must still be answered: a client that sends CAP LS waits for
    // a reply before sending NICK/USER, and a 421 here stalls the connection.
    if (msg.params.empty())
        return;

    std::string sub = IRC::toUpper(msg.params[0]);
    if (sub == "LS" || sub == "LIST")
        sendToClient(fd, ":ircserv CAP * " + sub + " :\r\n");
    else if (sub == "REQ")
        sendToClient(fd, ":ircserv CAP * NAK :"
            + (msg.params.size() > 1 ? msg.params[1] : "") + "\r\n");
    // CAP END needs no answer: registration simply continues.
}

void Server::handleWHO(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];
    std::string mask = msg.params.empty() ? "*" : msg.params[0];

    // "H" = here (as opposed to G, gone/away). "0" is the hop count: we have
    // no server links, so every user is zero hops away.
    if (!mask.empty() && (mask[0] == '#' || mask[0] == '&'))
    {
        Channel* channel = _findChannel(mask);
        if (channel)
        {
            const std::vector<Client*>& members = channel->getClients();
            for (size_t i = 0; i < members.size(); i++)
            {
                std::string flags = "H";
                if (channel->isOperator(members[i]))
                    flags += "@";
                sendToClient(fd, IRC::makeReply(IRC::RPL_WHOREPLY,
                    client->getNickname() + " " + channel->getName() + " "
                    + members[i]->getUsername() + " " + members[i]->getHostname()
                    + " ircserv " + members[i]->getNickname() + " " + flags,
                    "0 " + members[i]->getRealname()));
            }
        }
    }
    else
    {
        Client* target = _findClientByNick(mask);
        if (target)
            sendToClient(fd, IRC::makeReply(IRC::RPL_WHOREPLY,
                client->getNickname() + " * " + target->getUsername() + " "
                + target->getHostname() + " ircserv " + target->getNickname()
                + " H", "0 " + target->getRealname()));
    }

    sendToClient(fd, IRC::makeReply(IRC::RPL_ENDOFWHO,
        client->getNickname() + " " + mask, "End of /WHO list"));
}

void Server::handleWHOIS(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (msg.params.empty() || msg.params[0].empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NONICKNAMEGIVEN,
            client->getNickname(), "No nickname given"));
        return;
    }

    Client* target = _findClientByNick(msg.params[0]);
    if (!target)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHNICK,
            client->getNickname() + " " + msg.params[0], "No such nick/channel"));
        sendToClient(fd, IRC::makeReply(IRC::RPL_ENDOFWHOIS,
            client->getNickname() + " " + msg.params[0], "End of /WHOIS list"));
        return;
    }

    sendToClient(fd, IRC::makeReply(IRC::RPL_WHOISUSER,
        client->getNickname() + " " + target->getNickname() + " "
        + target->getUsername() + " " + target->getHostname() + " *",
        target->getRealname()));
    sendToClient(fd, IRC::makeReply(IRC::RPL_WHOISSERVER,
        client->getNickname() + " " + target->getNickname() + " ircserv",
        "ft_irc server"));

    std::string channels;
    for (std::map<std::string, Channel*>::iterator it = _channels.begin();
         it != _channels.end(); ++it)
    {
        if (it->second->hasClient(target))
        {
            if (!channels.empty())
                channels += " ";
            if (it->second->isOperator(target))
                channels += "@";
            channels += it->second->getName();
        }
    }
    if (!channels.empty())
        sendToClient(fd, IRC::makeReply(IRC::RPL_WHOISCHANNELS,
            client->getNickname() + " " + target->getNickname(), channels));

    sendToClient(fd, IRC::makeReply(IRC::RPL_ENDOFWHOIS,
        client->getNickname() + " " + target->getNickname(),
        "End of /WHOIS list"));
}

void Server::handleNAMES(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (msg.params.empty() || msg.params[0].empty())
    {
        for (std::map<std::string, Channel*>::iterator it = _channels.begin();
             it != _channels.end(); ++it)
            _sendNames(fd, it->second);
        sendToClient(fd, IRC::makeReply(IRC::RPL_ENDOFNAMES,
            client->getNickname() + " *", "End of /NAMES list"));
        return;
    }

    std::vector<std::string> names = _split(msg.params[0], ',');
    for (size_t i = 0; i < names.size(); i++)
    {
        Channel* channel = _findChannel(names[i]);
        if (channel)
            _sendNames(fd, channel);
        else
            sendToClient(fd, IRC::makeReply(IRC::RPL_ENDOFNAMES,
                client->getNickname() + " " + names[i], "End of /NAMES list"));
    }
}

void Server::handleLIST(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];
    (void)msg;

    sendToClient(fd, IRC::makeReply(IRC::RPL_LISTSTART,
        client->getNickname() + " Channel", "Users  Name"));

    for (std::map<std::string, Channel*>::iterator it = _channels.begin();
         it != _channels.end(); ++it)
    {
        std::ostringstream count;
        count << it->second->getClients().size();
        sendToClient(fd, IRC::makeReply(IRC::RPL_LIST,
            client->getNickname() + " " + it->second->getName() + " "
            + count.str(), it->second->getTopic()));
    }

    sendToClient(fd, IRC::makeReply(IRC::RPL_LISTEND,
        client->getNickname(), "End of /LIST"));
}