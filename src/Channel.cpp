
#include "Channel.hpp"

Channel::Channel(const std::string& name)
    : _name(name), _inviteOnly(false), _topicRestricted(false),
      _key(""), _userLimit(-1)
{}
Channel::~Channel() {}

bool Channel::hasClient(Client* client) const
{
    for (std::vector<Client*>::const_iterator it = _clients.begin();
         it != _clients.end();
         ++it)
    {
        if (*it == client)
            return true;
    }
    return false;
}


void Channel::addClient(Client* client)
{
    if (hasClient(client))
        return;

    _clients.push_back(client);

    if (_clients.size() == 1)
        addOperator(client);
}

void Channel::removeClient(Client* client)
{
    for (std::vector<Client*>::iterator it = _clients.begin(); it != _clients.end(); ++it)
    {
        if (*it == client)
        {
            _clients.erase(it);
            break;
        }
    }
    removeOperator(client);
}

bool Channel::isOperator(Client* client) const
{
    for (std::vector<Client*>::const_iterator it = _operators.begin(); it != _operators.end(); ++it)
    {
        if (*it == client)
            return true;
    }
    return false;
}

void Channel::addOperator(Client* client)
{
    if (!isOperator(client))
        _operators.push_back(client); 
}

void Channel::removeOperator(Client* client)
{
    for (std::vector<Client*>::iterator it = _operators.begin(); it != _operators.end(); ++it)
    {
        if (*it == client)
        {
            _operators.erase(it);
            break;
        }
    }
}

bool Channel::empty() const
{
    return _clients.empty();
}

const std::string& Channel::getTopic() const
{
    return _topic;
}

void Channel::setTopic(const std::string& topic)
{
    _topic = topic;
}
void Channel::broadcast(const std::string& msg, Client* sender)
{
    for (std::vector<Client*>::iterator it = _clients.begin(); it != _clients.end(); ++it)
    {
        if (*it != sender)
            (*it)->queueMessage(msg);
    }
}

const std::vector<Client*>& Channel::getClients() const
{
    return _clients;
}

void Channel::addInvited(Client* client)
{
    if (!isInvited(client))
        _invitedClients.push_back(client);
}

void Channel::removeInvited(Client* client)
{
    for (std::vector<Client*>::iterator it = _invitedClients.begin();
         it != _invitedClients.end(); ++it)
    {
        if (*it == client)
        {
            _invitedClients.erase(it);
            break;
        }
    }
}

bool Channel::isInvited(Client* client) const
{
    for (std::vector<Client*>::const_iterator it = _invitedClients.begin();
         it != _invitedClients.end(); ++it)
    {
        if (*it == client)
            return true;
    }
    return false;
}

bool Channel::isInviteOnly() const 
{
    return _inviteOnly;
}

void Channel::setInviteOnly(bool value) 
{
    _inviteOnly = value;
}

bool Channel::isTopicRestricted() const 
{
    return _topicRestricted;
}
void Channel::setTopicRestricted(bool value)
{
    _topicRestricted = value;
}

bool Channel::hasKey() const
{
    return !_key.empty();
}
void Channel::setKey(const std::string& key)
{
    _key = key;
}
bool Channel::checkKey(const std::string& key) const 
{
    return _key == key;
}

int Channel::getUserLimit() const 
{
    return _userLimit;
}
void Channel::setUserLimit(int limit)
{
    _userLimit = limit;
}
bool Channel::isFull() const
{
    return (_userLimit >= 0 && (int)_clients.size() >= _userLimit);
}

const std::string& Channel::getName() const 
{
    return _name;
}
