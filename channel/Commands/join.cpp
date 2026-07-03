#include "../Channel.hpp"

void Server::handleJOIN(int fd, IRCMessage& msg)
{
    if (msg.params.empty())
        throw std::runtime_error("ERR_NEEDMOREPARAMS");

    std::string channelName = msg.params[0];

    if (channelName.empty() || channelName[0] != '#')
        throw std::runtime_error("Invalid channel name");

    Channel* channel;

    std::map<std::string, Channel*>::iterator it = _channels.find(channelName);

    if (it == _channels.end())
    {
        channel = new Channel(channelName);
        _channels[channelName] = channel;
    }
    else
        channel = it->second;

    Client* client = _clients[fd];

    if (channel->hasClient(client))
        return;

   if (channel->isInviteOnly() && !channel->isInvited(client))
        throw std::runtime_error("ERR_INVITEONLYCHAN");

    if (channel->hasKey())
    {
        if (msg.params.size() < 2 || !channel->checkKey(msg.params[1]))
            throw std::runtime_error("ERR_BADCHANNELKEY");
    }

    if (channel->hasLimit() && channel->isFull())
        throw std::runtime_error("ERR_CHANNELISFULL");

    if (channel->empty())
        channel->addOperator(client);

    channel->addClient(client);

    channel->removeInvited(client);

    std::string joinMsg =
        ":" + client->getPrefix() +
        " JOIN " +
        channelName +
        "\r\n";

    channel->broadcast(joinMsg, NULL);

    if (channel->getTopic().empty())
    {
        client->queueMessage(
            ":ircserv 331 " +
            client->getNickname() +
            " " +
            channelName +
            " :No topic is set\r\n");
    }
    else
    {
        client->queueMessage(
            ":ircserv 332 " +
            client->getNickname() +
            " " +
            channelName +
            " :" +
            channel->getTopic() +
            "\r\n");
    }

    const std::vector<Client*>& clients = channel->getClients();

    std::string names;

    for (size_t i = 0; i < clients.size(); i++)
    {
        Client* c = clients[i];

        if (channel->isOperator(c))
            names += "@";

        names += c->getNickname();

        if (i + 1 < clients.size())
            names += " ";
    }

    client->queueMessage(
        ":ircserv 353 " +
        client->getNickname() +
        " = " +
        channelName +
        " :" +
        names +
        "\r\n");

    client->queueMessage(
        ":ircserv 366 " +
        client->getNickname() +
        " " +
        channelName +
        " :End of /NAMES list\r\n");
}
