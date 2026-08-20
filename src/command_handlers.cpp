#include "Server.hpp"

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

#include "Server.hpp"

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
