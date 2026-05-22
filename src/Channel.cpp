#include "Channel.hpp"
#include "Client.hpp"
#include <sstream>

Channel::Channel(const std::string& name)
    : _name(name), _userLimit(-1),
      _inviteOnly(false), _topicRestricted(false)
{}

Channel::~Channel() {}

void Channel::addMember(Client* client)
{
    _members[client->getFd()] = client;
}

void Channel::removeMember(int fd)
{
    _members.erase(fd);
    _operators.erase(fd);
    _invited.erase(fd);
}

bool Channel::isMember(int fd) const
{
    return _members.count(fd) > 0;
}

void Channel::addOperator(int fd)   { _operators.insert(fd); }
void Channel::removeOperator(int fd){ _operators.erase(fd); }
bool Channel::isOperator(int fd) const { return _operators.count(fd) > 0; }

void Channel::addInvited(int fd)    { _invited.insert(fd); }
bool Channel::isInvited(int fd) const { return _invited.count(fd) > 0; }

void Channel::broadcast(const std::string& msg, int excludeFd)
{
    for (std::map<int, Client*>::iterator it = _members.begin();
         it != _members.end(); ++it)
    {
        if (it->first != excludeFd)
            it->second->queueMessage(msg);
    }
}

std::string Channel::getMemberList() const
{
    std::string list;
    for (std::map<int, Client*>::const_iterator it = _members.begin();
         it != _members.end(); ++it)
    {
        if (!list.empty()) list += " ";
        if (_operators.count(it->first)) list += "@";
        list += it->second->getNickname();
    }
    return list;
}