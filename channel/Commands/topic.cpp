#include "../Channel.hpp"

void Server::handleTOPIC(int fd, IRCMessage& msg)
{
    if (msg.params.empty())
        throw std::runtime_error("ERR_NEEDMOREPARAMS");

    std::string channelName = msg.params[0];
    std::string newTopic = msg.params[1];


    std::map<std::string, Channel*>::iterator it = _channels.find(channelName);
    if (it == _channels.end())
        throw std::runtime_error("ERR_NOSUCHCHANNEL");

    Channel* channel = it->second;
    Client* client = _clients[fd];

    if (!channel->hasClient(client))
        throw std::runtime_error("ERR_NOTONCHANNEL");


    if (msg.params.size() == 1)
    {
        if (channel->getTopic().empty())
        {
            std::string msg331 =
                ":ircserv 331 " + client->getNickname() +
                " " + channelName +
                " :No topic is set\r\n";

            client->queueMessage(msg331);
        }
        else
        {
            std::string msg332 =
                ":ircserv 332 " + client->getNickname() +
                " " + channelName +
                " :" + channel->getTopic() + "\r\n";

            client->queueMessage(msg332);
        }
        return;
    }

    if (channel->isTopicRestricted() && !channel->isOperator(client))
    {
        std::string err =
            ":ircserv 482 " + client->getNickname() +
            " " + channelName +
            " :You're not channel operator\r\n";

        client->queueMessage(err);
        return;
    }

    std::string newTopic = msg.params[1];

    if (!newTopic.empty() && newTopic[0] == ':')
        newTopic.erase(0, 1);

    channel->setTopic(newTopic);

    std::string topicMsg =
        ":" + client->getNickname() +
        "!" + client->getUsername() +
        " TOPIC " + channelName +
        " :" + newTopic + "\r\n";

    channel->broadcast(topicMsg, NULL);
}
