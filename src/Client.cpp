
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


// #include "Client.hpp"

// Client::Client(int fd){
//     this->_fd = fd;
// }


// void    Client::appendToBuffer(const std::string& data){
//     this->_readBuffer += data;
// }

// bool    Client::getNextMessage(std::string& msg)
// {
//     std::size_t pos = this->_readBuffer.find("\r\n");
//     if(pos == std::string::npos){
//         return false;
//     }
//     msg = this->_readBuffer.substr(0, pos);
//     this->_readBuffer.erase(0, pos + 2);
//     return true;
// }

// void               Client::queueMessage(const std::string& msg)
// {
//     this->_writeBuffer += msg;
// }
// const std::string& Client::getWriteBuffer() const
// {
//     return this->_writeBuffer;
// }
// void               Client::clearWriteBuffer(int sentBytes)
// {
//     this->_writeBuffer.erase(0, sentBytes);
// }



// int         Client::getFd() const
// {
//     return this->_fd;
// }
// std::string Client::getNickname() const
// {
//     return this->_nickname;
// }
// std::string Client::getUsername() const
// {
//     return this->_username;
// }
// std::string Client::getRealname() const
// {
//     return this->_realname;
// }
// std::string Client::getHostname() const
// {
//     return this->_hostname;
// }

// std::string Client::getPrefix() const
// {
//     return this->_nickname + "!" + this->_username + "@" + this->_hostname;
// }


// bool Client::isPassAccepted() const
// {
//     if (this->_passAccepted)
//         return true;
//     return false
// }
// bool Client::isNickSet() const
// {
//     if (this->_nickSet)
//         return true;
//     return false;
// }
// bool Client::isUserSet() const
// {
//     if (this->_userSet)
//         return true;
//     return false;
// }
// bool Client::isFullyRegistered() const
// {
//     if(this->_passAccepted && this->_nickSet && this->_userSet)
//         return true;
//     return false;
// }


// void Client::setNickname(const std::string& nick)
// {
//     this->_nickname = nick;
// }
// void Client::setUsername(const std::string& user)
// {
//     this->_username = user;
// }
// void Client::setRealname(const std::string& real)
// {
//     this->_realname = real;
// }
// void Client::setHostname(const std::string& host)
// {
//     this->_hostname = host;
// }
// void Client::setPassAccepted(bool val)
// {
//     this->_passAccepted = val;
// }
// void Client::setNickSet(bool val)
// {
//     this->_nickSet = val;
// }
// void Client::setUserSet(bool val)
// {
//     this->_userSet = val;
// }
