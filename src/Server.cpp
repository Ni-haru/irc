#include "Server.hpp"

#include <cstdlib>
#include <cerrno>

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
    for (std::map<std::string, Channel*>::iterator it = _channels.begin();
         it != _channels.end(); ++it)
        delete it->second;

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
        int ready = poll(_fds.data(), _fds.size(), -1);

        if (ready == -1)
        {
            if (Server::_stop) break;  
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

        // Arm/disarm POLLOUT dynamically for each client at end of loop
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

    _clients[clientFd] = new Client(clientFd);

    std::cout << "New client connected: fd=" << clientFd << std::endl;
}

void Server::_readFromClient(int fd)
{
    char buf[512];
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
    while (_clients.count(fd) && !_closing.count(fd) && _clients[fd]->getNextMessage(msg))
        _handleMessage(fd, msg);
}

void Server::_flushWriteBuffer(int fd)
{
    if (!_clients.count(fd)) return;
    Client* client = _clients[fd];
    const std::string& buf = client->getWriteBuffer();

    if (buf.empty())
        return;

    int sent = send(fd, buf.c_str(), buf.size(), 0);
    if (sent > 0)
    {
        client->clearWriteBuffer(sent);
        if (client->getWriteBuffer().empty() && _closing.count(fd))
            _disconnectClient(fd);
    }
    else
        _disconnectClient(fd);
}

// Ask for a disconnect that must NOT lose the bytes still queued for the client.
// If nothing is pending we close right away; otherwise we remember the fd and
// close it in _reapClosing() once _flushWriteBuffer() has drained the buffer.
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

// Called once per poll() iteration: closes every client that was marked for
// disconnection and has since finished writing.
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
        std::map<std::string, Channel*>::iterator ch = _channels.begin();
        while (ch != _channels.end())
        {
            ch->second->removeClient(client);
            ch->second->removeOperator(client);
            ch->second->removeInvited(client);
            if (ch->second->empty())
            {
                delete ch->second;
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
 
void Server::sendToAll(const std::string& msg)
{
    for (std::map<int, Client*>::iterator it = _clients.begin();
         it != _clients.end(); ++it)
        sendToClient(it->first, msg);
}

void Server::handlePING(int fd, IRCMessage& msg)
{
    std::string token = msg.params.empty() ? "ircserv" : msg.params[0];
    sendToClient(fd, "PONG :ircserv " + token + "\r\n");
}

void Server::_handleMessage(int fd, const std::string& raw)
{
    if (raw.empty()) return;
 
    IRCMessage msg = _parseMessage(raw);
    if (msg.command.empty()) return;
 
    std::cout << "fd=" << fd
              << " cmd=" << msg.command
              << " params=" << msg.params.size()
              << std::endl;

    try
    {
        if      (msg.command == "PASS")    handlePASS(fd, msg);
        else if (msg.command == "NICK")    handleNICK(fd, msg);
        else if (msg.command == "USER")    handleUSER(fd, msg);
        else if (msg.command == "PING")    handlePING(fd, msg);
        else if (msg.command == "QUIT")    handleQUIT(fd, msg);
        else if (msg.command == "PART")    handlePART(fd, msg);
        else if (msg.command == "PRIVMSG") handlePRIVMSG(fd, msg);
        else if (msg.command == "JOIN")    handleJOIN(fd, msg);
        else if (msg.command == "KICK")    handleKICK(fd, msg);
        else if (msg.command == "INVITE")  handleINVITE(fd, msg);
        else if (msg.command == "TOPIC")   handleTOPIC(fd, msg);
        else if (msg.command == "MODE")    handleMODE(fd, msg);
        else
        {
            if (_clients.count(fd))
            {
                std::string nick = _clients[fd]->getNickname();
                if (nick.empty()) nick = "*";
                sendToClient(fd, IRC::makeReply(IRC::ERR_UNKNOWNCOMMAND,
                    nick, msg.command + " :Unknown command"));
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error handling message on fd " << fd << ": " << e.what() << std::endl;
    }
}

void Server::sendToClient(int fd, const std::string& msg)
{
    if (!_clients.count(fd)) return;
    _clients[fd]->queueMessage(msg);
}

void Server::handlePASS(int fd, IRCMessage& msg)
{
    Client* client = this->_clients[fd];
    if (client->isFullyRegistered())
    {
        std::string nick = client->getNickname().empty() ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_ALREADYREGISTRED, nick, "You may not reregister"));
        return;
    }
    else if (msg.params.empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS, "* " + msg.command, "Not enough parameters"));
        return;
    }
    else if (msg.params[0] != this->_password)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_PASSWDMISMATCH, "*", "Password incorrect"));
        this->_queueDisconnect(fd);
        return;
    }
    else
        client->setPassAccepted(true);
}

void Server::sendWelcome(Client* client)
{
    this->sendToClient(client->getFd(), IRC::makeReply(IRC::RPL_WELCOME, client->getNickname(), "Welcome to the IRC Network " + client->getPrefix()));
    this->sendToClient(client->getFd(), IRC::makeReply(IRC::RPL_YOURHOST, client->getNickname(), "Your host is ircserv running version 1.0"));
    this->sendToClient(client->getFd(), IRC::makeReply(IRC::RPL_CREATED, client->getNickname(), "This server was created today"));
    this->sendToClient(client->getFd(), ":ircserv 004 " +  client->getNickname() + " ircserv 1.0 o iklt\r\n");
}

void Server::handleNICK(int fd, IRCMessage& msg)
{
    Client* client = this->_clients[fd];
    if (!client->isPassAccepted())
    {
        std::string nick = client->getNickname().empty() ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTREGISTERED, nick, "You have not registered"));
        return;
    }
    if (msg.params.empty() || msg.params[0].empty())
    {
        std::string nick = client->getNickname().empty() ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_NONICKNAMEGIVEN, nick, "No nickname given"));
        return;
    }

    std::string newNick = msg.params[0];
    bool invalidChar = false;
    std::string autoris = "[]{}\\|-_^";
    
    for (std::size_t i = 0; i < newNick.size(); i++)
    {
        if (!std::isalnum(newNick[i]) && autoris.find(newNick[i]) == std::string::npos)
        {
            invalidChar = true;
            break;
        }
    }
    
    if (invalidChar || newNick.size() > 9 || newNick.empty() || std::isdigit(newNick[0]))
    {
        std::string nick = client->getNickname().empty() ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_ERRONEUSNICK, nick + " " + newNick, "Erroneous nickname"));
        return;
    }

    for (std::map<int, Client*>::iterator it = this->_clients.begin(); it != this->_clients.end(); it++)
    {
        if (it->first != fd && it->second->getNickname() == newNick)
        {
            std::string nick = client->getNickname().empty() ? "*" : client->getNickname();
            sendToClient(fd, IRC::makeReply(IRC::ERR_NICKNAMEINUSE, nick + " " + newNick, "Nickname is already in use"));
            return;
        }
    }

    std::string oldNick = client->getNickname();
    client->setNickname(newNick);
    client->setNickSet(true);

    if (!client->isFullyRegistered())
    {
        if (client->isFullyRegistered())
            this->sendWelcome(client);
    }
    else
    {
        std::string nickMsg = ":" + client->getPrefix() + " NICK " + newNick + "\r\n";
        sendToClient(fd, nickMsg);
        
        for (std::map<std::string, Channel*>::iterator it = _channels.begin(); it != _channels.end(); ++it)
        {
            if (it->second->hasClient(client))
                it->second->broadcast(nickMsg, client);
        }
    }
}

void Server::handleUSER(int fd, IRCMessage& msg)
{
    Client* client = this->_clients[fd];
    if (!client->isPassAccepted())
    {
        std::string nick = client->getNickname().empty() ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTREGISTERED, nick, "You have not registered"));
        return;
    }
    if (client->isUserSet())
    {
        std::string nick = client->getNickname().empty() ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_ALREADYREGISTRED, nick, "You may not reregister"));
        return;
    }
    if (msg.params.size() < 4)
    {
        std::string nick = client->getNickname().empty() ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS, nick + " " + msg.command, "Not enough parameters"));
        return;
    }

    client->setUsername(msg.params[0]);
    client->setRealname(msg.params[3]);
    client->setUserSet(true);
    if (client->isFullyRegistered())
        this->sendWelcome(client);
}

void Server::handleQUIT(int fd, IRCMessage& msg)
{
    Client* client = this->_clients[fd];
    if (client->isFullyRegistered())
    {
        std::string raison = msg.params.empty() ? "Quit" : msg.params[0];
        std::string quitMsg = ":" + client->getPrefix() + " QUIT :" + raison + "\r\n";
        
        for (std::map<std::string, Channel*>::iterator it = _channels.begin(); it != _channels.end(); ++it)
        {
            if (it->second->hasClient(client))
                it->second->broadcast(quitMsg, client);
        }
    }
    sendToClient(fd, "ERROR :Closing connection\r\n");
    this->_queueDisconnect(fd);
}

void Server::handlePART(int fd, IRCMessage& msg)
{
    Client* client = this->_clients[fd];
    if (!client->isFullyRegistered())
    {
        std::string nick = client->getNickname().empty() ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTREGISTERED, nick, "You have not registered"));
        return;
    }
    if (msg.params.empty() || msg.params[0].empty())
    {
        std::string nick = client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS, nick + " " + msg.command, "Not enough parameters"));
        return;
    }

    std::stringstream ss(msg.params[0]);
    std::string channel;
    std::string reason = (msg.params.size() > 1) ? msg.params[1] : "";

    while (std::getline(ss, channel, ','))
    {
        std::map<std::string, Channel*>::iterator it = _channels.find(channel);
        if (it == _channels.end())
        {
            sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL, client->getNickname() + " " + channel, "No such channel"));
        }
        else if (!it->second->hasClient(client))
        {
            sendToClient(fd, IRC::makeReply(IRC::ERR_NOTONCHANNEL, client->getNickname() + " " + channel, "You're not on that channel"));
        }
        else
        {
            std::string partMsg = ":" + client->getPrefix() + " PART " + channel;
            if (!reason.empty()) partMsg += " :" + reason;
            partMsg += "\r\n";

            it->second->broadcast(partMsg, NULL);
            it->second->removeClient(client);
            it->second->removeOperator(client);
            if (it->second->empty())
            {
                delete it->second;
                _channels.erase(it);
            }
        }
    }
}

void Server::handlePRIVMSG(int fd, IRCMessage& msg)
{
    Client* client = this->_clients[fd];
    if (!client->isFullyRegistered())
    {
        std::string nick = client->getNickname().empty() ? "*" : client->getNickname();
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTREGISTERED, nick, "You have not registered"));
        return;
    }
    if (msg.params.empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NORECIPIENT, client->getNickname(), "No recipient given (" + msg.command + ")"));
        return;
    }
    if (msg.params.size() < 2 || msg.params[1].empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTEXTTOSEND, client->getNickname(), "No text to send"));
        return;
    }

    std::stringstream ss(msg.params[0]);
    std::string target;
    while (std::getline(ss, target, ','))
    {
        if (target[0] == '#' || target[0] == '&')
        {
            std::map<std::string, Channel*>::iterator it = _channels.find(target);
            if (it == _channels.end())
            {
                sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL, client->getNickname() + " " + target, "No such channel"));
            }
            else if (!it->second->hasClient(client))
            {
                sendToClient(fd, IRC::makeReply(IRC::ERR_CANNOTSENDTOCHAN, client->getNickname() + " " + target, "Cannot send to channel"));
            }
            else
            {
                std::string fullMsg = ":" + client->getPrefix() + " PRIVMSG " + target + " :" + msg.params[1] + "\r\n";
                it->second->broadcast(fullMsg, client);
            }
        }
        else
        {
            Client* targetClient = NULL;
            for (std::map<int, Client*>::iterator cit = _clients.begin(); cit != _clients.end(); ++cit)
            {
                if (cit->second->getNickname() == target)
                {
                    targetClient = cit->second;
                    break;
                }
            }
            if (!targetClient)
            {
                sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHNICK, client->getNickname() + " " + target, "No such nick/channel"));
            }
            else
            {
                std::string fullMsg = ":" + client->getPrefix() + " PRIVMSG " + target + " :" + msg.params[1] + "\r\n";
                sendToClient(targetClient->getFd(), fullMsg);
            }
        }
    }
}

void Server::handleJOIN(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];
    if (msg.params.empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS, client->getNickname() + " JOIN", "Not enough parameters"));
        return;
    }

    std::string channelName = msg.params[0];
    if (channelName.empty() || channelName[0] != '#')
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL, client->getNickname() + " " + channelName, "No such channel"));
        return;
    }

    Channel* channel;
    std::map<std::string, Channel*>::iterator it = _channels.find(channelName);

    if (it == _channels.end())
    {
        channel = new Channel(channelName);
        _channels[channelName] = channel;
    }
    else
        channel = it->second;

    if (channel->hasClient(client))
        return;

    if (channel->isInviteOnly() && !channel->isInvited(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_INVITEONLYCHAN, client->getNickname() + " " + channelName, "Cannot join channel (+i)"));
        return;
    }

    if (channel->hasKey())
    {
        if (msg.params.size() < 2 || !channel->checkKey(msg.params[1]))
        {
            sendToClient(fd, IRC::makeReply(IRC::ERR_BADCHANNELKEY, client->getNickname() + " " + channelName, "Cannot join channel (+k)"));
            return;
        }
    }

    if (channel->hasLimit() && channel->isFull())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_CHANNELISFULL, client->getNickname() + " " + channelName, "Cannot join channel (+l)"));
        return;
    }

    if (channel->empty())
        channel->addOperator(client);

    channel->addClient(client);
    channel->removeInvited(client);

    std::string joinMsg = ":" + client->getPrefix() + " JOIN " + channelName + "\r\n";
    channel->broadcast(joinMsg, NULL);

    if (channel->getTopic().empty())
        sendToClient(fd, ":ircserv 331 " + client->getNickname() + " " + channelName + " :No topic is set\r\n");
    else
        sendToClient(fd, ":ircserv 332 " + client->getNickname() + " " + channelName + " :" + channel->getTopic() + "\r\n");

    const std::vector<Client*>& clients = channel->getClients();
    std::string names;
    for (size_t i = 0; i < clients.size(); i++)
    {
        if (channel->isOperator(clients[i]))
            names += "@";
        names += clients[i]->getNickname();
        if (i + 1 < clients.size())
            names += " ";
    }

    sendToClient(fd, ":ircserv 353 " + client->getNickname() + " = " + channelName + " :" + names + "\r\n");
    sendToClient(fd, ":ircserv 366 " + client->getNickname() + " " + channelName + " :End of /NAMES list\r\n");
}

void Server::handleKICK(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];
    if (msg.params.size() < 2)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS, client->getNickname() + " KICK", "Not enough parameters"));
        return;
    }

    std::string channelName = msg.params[0];
    std::string targetNick = msg.params[1];

    std::map<std::string, Channel*>::iterator channelIt = _channels.find(channelName);
    if (channelIt == _channels.end())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL, client->getNickname() + " " + channelName, "No such channel"));
        return;
    }

    Channel* channel = channelIt->second;

    if (!channel->hasClient(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTONCHANNEL, client->getNickname() + " " + channelName, "You're not on that channel"));
        return;
    }

    if (!channel->isOperator(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_CHANOPRIVSNEEDED, client->getNickname() + " " + channelName, "You're not channel operator"));
        return;
    }

    Client* target = NULL;
    for (std::map<int, Client*>::iterator clientIt = _clients.begin(); clientIt != _clients.end(); ++clientIt)
    {
        if (clientIt->second->getNickname() == targetNick)
        {
            target = clientIt->second;
            break;
        }
    }

    if (target == NULL)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHNICK, client->getNickname() + " " + targetNick, "No such nick/channel"));
        return;
    }

    if (!channel->hasClient(target))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_USERNOTINCHANNEL, client->getNickname() + " " + targetNick + " " + channelName, "They aren't on that channel"));
        return;
    }

    std::string kickMsg = ":" + client->getPrefix() + " KICK " + channelName + " " + targetNick;
    if (msg.params.size() > 2)
        kickMsg += " :" + msg.params[2];
    kickMsg += "\r\n";

    channel->broadcast(kickMsg, NULL);
    channel->removeClient(target);
    channel->removeOperator(target);

    if (channel->empty())
    {
        delete channel;
        _channels.erase(channelIt);
    }
}

void Server::handleINVITE(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];
    if (msg.params.size() < 2)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS, client->getNickname() + " INVITE", "Not enough parameters"));
        return;
    }

    std::string targetNick = msg.params[0];
    std::string channelName = msg.params[1];

    std::map<std::string, Channel*>::iterator channelIt = _channels.find(channelName);
    if (channelIt == _channels.end())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL, client->getNickname() + " " + channelName, "No such channel"));
        return;
    }

    Channel* channel = channelIt->second;

    if (!channel->hasClient(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTONCHANNEL, client->getNickname() + " " + channelName, "You're not on that channel"));
        return;
    }

    if (channel->isInviteOnly() && !channel->isOperator(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_CHANOPRIVSNEEDED, client->getNickname() + " " + channelName, "You're not channel operator"));
        return;
    }

    Client* target = NULL;
    for (std::map<int, Client*>::iterator clientIt = _clients.begin(); clientIt != _clients.end(); ++clientIt)
    {
        if (clientIt->second->getNickname() == targetNick)
        {
            target = clientIt->second;
            break;
        }
    }

    if (target == NULL)
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHNICK, client->getNickname() + " " + targetNick, "No such nick/channel"));
        return;
    }

    if (channel->hasClient(target))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_USERONCHANNEL, client->getNickname() + " " + targetNick + " " + channelName, "is already on channel"));
        return;
    }

    if (!channel->isInvited(target))
        channel->addInvited(target);

    std::string inviteMsg = ":" + client->getPrefix() + " INVITE " + targetNick + " " + channelName + "\r\n";
    std::string reply = ":ircserv 341 " + client->getNickname() + " " + targetNick + " " + channelName + "\r\n";

    sendToClient(target->getFd(), inviteMsg);
    sendToClient(client->getFd(), reply);
}

void Server::handleMODE(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];
    if (msg.params.empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS, client->getNickname() + " MODE", "Not enough parameters"));
        return;
    }

    std::string channelName = msg.params[0];
    std::map<std::string, Channel*>::iterator it = _channels.find(channelName);
    if (it == _channels.end())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL, client->getNickname() + " " + channelName, "No such channel"));
        return;
    }

    Channel* channel = it->second;
    if (!channel->hasClient(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTONCHANNEL, client->getNickname() + " " + channelName, "You're not on that channel"));
        return;
    }

    if (msg.params.size() == 1)
        return;

    if (!channel->isOperator(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_CHANOPRIVSNEEDED, client->getNickname() + " " + channelName, "You're not channel operator"));
        return;
    }

    std::string modes = msg.params[1];
    char sign = '+';
    int argIndex = 2;

    for (size_t i = 0; i < modes.size(); i++)
    {
        char c = modes[i];
        if (c == '+' || c == '-')
        {
            sign = c;
            continue;
        }

        if (c == 'i')
            channel->setInviteOnly(sign == '+');
        else if (c == 't')
            channel->setTopicRestricted(sign == '+');
        else if (c == 'k')
        {
            if (sign == '+')
            {
                if (argIndex >= (int)msg.params.size())
                {
                    sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS, client->getNickname() + " MODE", "Not enough parameters"));
                    return;
                }
                channel->setKey(msg.params[argIndex++]);
            }
            else
                channel->setKey("");
        }
        else if (c == 'l')
        {
            if (sign == '+')
            {
                if (argIndex >= (int)msg.params.size())
                {
                    sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS, client->getNickname() + " MODE", "Not enough parameters"));
                    return;
                }
                int limit = std::atoi(msg.params[argIndex++].c_str());
                if (limit > 0)
                    channel->setUserLimit(limit);
            }
            else
                channel->setUserLimit(-1);
        }
        else if (c == 'o')
        {
            if (argIndex >= (int)msg.params.size())
            {
                sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS, client->getNickname() + " MODE", "Not enough parameters"));
                return;
            }

            std::string targetNick = msg.params[argIndex++];
            Client* target = NULL;

            for (std::map<int, Client*>::iterator itc = _clients.begin(); itc != _clients.end(); ++itc)
            {
                if (itc->second->getNickname() == targetNick)
                {
                    target = itc->second;
                    break;
                }
            }

            if (!target || !channel->hasClient(target))
            {
                sendToClient(fd, IRC::makeReply(IRC::ERR_USERNOTINCHANNEL, client->getNickname() + " " + targetNick + " " + channelName, "They aren't on that channel"));
                return;
            }

            if (sign == '+')
                channel->addOperator(target);
            else
                channel->removeOperator(target);
        }
    }

    std::string modeMsg = ":" + client->getPrefix() + " MODE " + channelName + " " + modes;
    for (size_t i = 2; i < msg.params.size(); i++)
        modeMsg += " " + msg.params[i];
    modeMsg += "\r\n";

    channel->broadcast(modeMsg, NULL);
}

void Server::handleTOPIC(int fd, IRCMessage& msg)
{
    Client* client = _clients[fd];
    if (msg.params.empty())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NEEDMOREPARAMS, client->getNickname() + " TOPIC", "Not enough parameters"));
        return;
    }

    std::string channelName = msg.params[0];
    std::map<std::string, Channel*>::iterator it = _channels.find(channelName);
    if (it == _channels.end())
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOSUCHCHANNEL, client->getNickname() + " " + channelName, "No such channel"));
        return;
    }

    Channel* channel = it->second;

    if (!channel->hasClient(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_NOTONCHANNEL, client->getNickname() + " " + channelName, "You're not on that channel"));
        return;
    }

    if (msg.params.size() == 1)
    {
        if (channel->getTopic().empty())
            sendToClient(fd, ":ircserv 331 " + client->getNickname() + " " + channelName + " :No topic is set\r\n");
        else
            sendToClient(fd, ":ircserv 332 " + client->getNickname() + " " + channelName + " :" + channel->getTopic() + "\r\n");
        return;
    }

    std::string newTopic = msg.params[1];  

    if (channel->isTopicRestricted() && !channel->isOperator(client))
    {
        sendToClient(fd, IRC::makeReply(IRC::ERR_CHANOPRIVSNEEDED, client->getNickname() + " " + channelName, "You're not channel operator"));
        return;
    }

    channel->setTopic(newTopic);

    std::string topicMsg = ":" + client->getPrefix() + " TOPIC " + channelName + " :" + newTopic + "\r\n";
    channel->broadcast(topicMsg, NULL);
}