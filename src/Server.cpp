#include "Server.hpp"

#include <cstdlib>
#include <sstream>

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
    Client* holder = _findClientByNick(newNick);
    if (holder && holder != client)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NICKNAMEINUSE,
            current + " " + newNick, "Nickname is already in use"));
        return;
    }
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
    sendToClient(fd, ":ircserv PONG ircserv :" + msg.params[0] + "\r\n");
}

void Server::handlePONG(int fd, IRCMessage& msg)
{
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


void Server::handleJOIN(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (msg.params.empty() || msg.params[0].empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
            client->getNickname() + " JOIN", "Not enough parameters"));
        return;
    }

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
        return;                    

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

    channel->addClient(client);       
    channel->removeInvited(client);    
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


void Server::handleMODE(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];

    if (msg.params.empty() || msg.params[0].empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS,
            client->getNickname() + " MODE", "Not enough parameters"));
        return;
    }

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
                    continue;            
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

    if (IRC::toLower(msg.params[0]) != IRC::toLower(client->getNickname()))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_USERSDONTMATCH,
            client->getNickname(), "Cannot change mode for other users"));
        return;
    }
    sendToClient(fd, IRC::makeRawReply(IRC::RPL_UMODEIS,
        client->getNickname() + " +i"));
}

void Server::handleCAP(int fd, IRCMessage& msg)
{
    if (msg.params.empty())
        return;

    std::string sub = IRC::toUpper(msg.params[0]);
    if (sub == "LS" || sub == "LIST")
        sendToClient(fd, ":ircserv CAP * " + sub + " :\r\n");
    else if (sub == "REQ")
        sendToClient(fd, ":ircserv CAP * NAK :"
            + (msg.params.size() > 1 ? msg.params[1] : "") + "\r\n");
}

void Server::handleWHO(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];
    std::string mask = msg.params.empty() ? "*" : msg.params[0];

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