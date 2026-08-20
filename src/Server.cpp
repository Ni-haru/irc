#include "Server.hpp"

bool Server::_stop = false;

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

void Server::run()
{
    while (!Server::_stop)
    {
        int ready = poll(&_fds[0], _fds.size(), -1);

        if (ready == -1)
        {
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
                i--;                
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
    client->setHostname(inet_ntoa(clientAddr.sin_addr));
    _clients[clientFd] = client;

    std::cout << "New client connected: fd=" << clientFd << std::endl;
}

void Server::_readFromClient(int fd)
{
    char    buf[4096];
    ssize_t bytes = recv(fd, buf, sizeof(buf), 0);

    if (bytes <= 0)
    {
        _disconnectClient(fd);
        return;
    }

    std::map<int, Client*>::iterator it = _clients.find(fd);
    if (it == _clients.end())
        return;
    it->second->appendToBuffer(std::string(buf, bytes));
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
        client->clearWriteBuffer(static_cast<int>(sent));
        if (client->getWriteBuffer().empty() && _closing.count(fd))
            _disconnectClient(fd);
    }
    else
        _disconnectClient(fd);
    
}

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
                _channels.erase(ch++);  
            }                           
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
    addr.sin_addr.s_addr = INADDR_ANY;  
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
    while (!line.empty() && line[0] == ' ')
        line.erase(0, 1);

    pos = line.find(' ');
    if (pos == std::string::npos) // command_handlers.cpp
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
            args += " *";              
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

void Server::_handleMessage(int fd, const std::string& raw)
{
    if (raw.empty())
        return;

    IRCMessage msg = _parseMessage(raw);
    if (msg.command.empty())
        return;
    std::string cmd = IRC::toUpper(msg.command);
    msg.command = cmd;

    if (!_clients.count(fd))
        return;
    Client* client = _clients[fd];

    try
    {
        if      (cmd == "CAP")  { handleCAP(fd, msg);  return; }
        else if (cmd == "PASS") { handlePASS(fd, msg); return; }
        else if (cmd == "NICK") { handleNICK(fd, msg); return; }
        else if (cmd == "USER") { handleUSER(fd, msg); return; }
        else if (cmd == "QUIT") { handleQUIT(fd, msg); return; }
        else if (cmd == "PING") { handlePING(fd, msg); return; }
        else if (cmd == "PONG") { handlePONG(fd, msg); return; }

        if (!client->isFullyRegistered())
        {
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
        std::cerr << "Error handling message on fd " << fd
                  << ": " << e.what() << std::endl;
    }
}
