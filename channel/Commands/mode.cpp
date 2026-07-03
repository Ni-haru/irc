#include "../Channel.hpp"
#include <cstdlib>

void Server::handleMODE(int fd, IRCMessage& msg)
{
    if (msg.params.size() < 2)
        throw std::runtime_error("ERR_NEEDMOREPARAMS");

    std::string channelName = msg.params[0];
    std::string modes = msg.params[1];

    std::map<std::string, Channel*>::iterator it = _channels.find(channelName);
    if (it == _channels.end())
        throw std::runtime_error("ERR_NOSUCHCHANNEL");

    Channel* channel = it->second;
    Client* client = _clients[fd];

    if (!channel->hasClient(client))
        throw std::runtime_error("ERR_NOTONCHANNEL");

    if (!channel->isOperator(client))
        throw std::runtime_error("ERR_CHANOPRIVSNEEDED");

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
        {
            channel->setInviteOnly(sign == '+');
        }
        else if (c == 't')
        {
            channel->setTopicRestricted(sign == '+');
        }
        else if (c == 'k')
        {
            if (sign == '+')
            {
                if (argIndex >= (int)msg.params.size())
                    throw std::runtime_error("ERR_NEEDMOREPARAMS");

                channel->setKey(msg.params[argIndex++]);
            }
            else
            {
                channel->setKey("");
            }
        }
        else if (c == 'l')
        {
            if (sign == '+')
            {
                if (argIndex >= (int)msg.params.size())
                    throw std::runtime_error("ERR_NEEDMOREPARAMS");

                int limit = std::atoi(msg.params[argIndex++].c_str());

                if (limit <= 0)
                    throw std::runtime_error("ERR_INVALIDMODEPARAM");

                channel->setUserLimit(limit);
            }
            else
            {
                channel->setUserLimit(-1);
            }
        }
        else if (c == 'o')
        {
            if (argIndex >= (int)msg.params.size())
                throw std::runtime_error("ERR_NEEDMOREPARAMS");

            std::string targetNick = msg.params[argIndex++];

            Client* target = NULL;

            for (std::map<int, Client*>::iterator itc = _clients.begin();
                 itc != _clients.end(); ++itc)
            {
                if (itc->second->getNickname() == targetNick)
                {
                    target = itc->second;
                    break;
                }
            }

            if (target == NULL)
                throw std::runtime_error("ERR_NOSUCHNICK");

            if (!channel->hasClient(target))
                throw std::runtime_error("ERR_USERNOTINCHANNEL");

            if (sign == '+')
                channel->addOperator(target);
            else
                channel->removeOperator(target);
        }
    }

    std::string modeMsg =
        ":" + client->getPrefix() +
        " MODE " +
        channelName +
        " " +
        modes;

    for (size_t i = 2; i < msg.params.size(); i++)
        modeMsg += " " + msg.params[i];

    modeMsg += "\r\n";

    channel->broadcast(modeMsg, NULL);
}
