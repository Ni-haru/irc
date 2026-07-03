#include "../Channel.hpp"

void Server::handleKICK(int fd, IRCMessage& msg)
{
    if (msg.params.size() < 2)
        throw std::runtime_error("ERR_NEEDMOREPARAMS");

    std::string channelName = msg.params[0];
    std::string targetNick = msg.params[1];

    Client* client = _clients[fd];

    std::map<std::string, Channel*>::iterator channelIt = _channels.find(channelName);
    if (channelIt == _channels.end())
        throw std::runtime_error("ERR_NOSUCHCHANNEL");

    Channel* channel = channelIt->second;

    if (!channel->hasClient(client))
        throw std::runtime_error("ERR_NOTONCHANNEL");

    if (!channel->isOperator(client))
        throw std::runtime_error("ERR_CHANOPRIVSNEEDED");

    Client* target = NULL;

    for (std::map<int, Client*>::iterator clientIt = _clients.begin();
         clientIt != _clients.end(); ++clientIt)
    {
        Client* member = clientIt->second;

        if (member->getNickname() == targetNick)
        {
            target = member;
            break;
        }
    }

    if (target == NULL)
        throw std::runtime_error("ERR_NOSUCHNICK");

    if (!channel->hasClient(target))
        throw std::runtime_error("ERR_USERNOTINCHANNEL");

    std::string kickMsg;

    if (msg.params.size() == 2)
    {
        kickMsg =
            ":" + client->getPrefix() +
            " KICK " +
            channelName +
            " " +
            targetNick +
            "\r\n";
    }
    else
    {
        kickMsg =
            ":" + client->getPrefix() +
            " KICK " +
            channelName +
            " " +
            targetNick +
            " :" +
            msg.params[2] +
            "\r\n";
    }

    sendToClient(client->getFd(), kickMsg);

    channel->broadcast(kickMsg, client);

    channel->removeClient(target);

    channel->removeOperator(target);

    if (channel->empty())
    {
        delete channel;
        _channels.erase(channelIt);
    }
}
