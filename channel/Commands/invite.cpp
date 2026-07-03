#include "../Channel.hpp"

void Server::handleINVITE(int fd, IRCMessage& msg)
{
    if (msg.params.size() != 2)
        throw std::runtime_error("ERR_NEEDMOREPARAMS");

    std::string targetNick = msg.params[0];
    std::string channelName = msg.params[1];

    Client* client = _clients[fd];

    std::map<std::string, Channel*>::iterator channelIt = _channels.find(channelName);
    if (channelIt == _channels.end())
        throw std::runtime_error("ERR_NOSUCHCHANNEL");

    Channel* channel = channelIt->second;

    if (!channel->hasClient(client))
        throw std::runtime_error("ERR_NOTONCHANNEL");

    if (channel->isInviteOnly() && !channel->isOperator(client))
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

    if (channel->hasClient(target))
        throw std::runtime_error("ERR_USERONCHANNEL");

    if (!channel->isInvited(target))
        channel->addInvited(target);

    std::string inviteMsg =
        ":" + client->getPrefix() +
        " INVITE " +
        targetNick +
        " " +
        channelName +
        "\r\n";

    std::string reply =
        ":server 341 " +
        client->getNickname() +
        " " +
        targetNick +
        " " +
        channelName +
        "\r\n";

    sendToClient(target->getFd(), inviteMsg);
    sendToClient(client->getFd(), reply);
}
