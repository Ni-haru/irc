#include "Client.hpp"

Client::Client(int fd)
    : _fd(fd), _hostname("127.0.0.1"),
      _passAccepted(false), _nickSet(false), _userSet(false)
{}

Client::~Client()
{
}


void    Client::appendToBuffer(const std::string& data){
    this->_readBuffer += data;
}

bool    Client::getNextMessage(std::string& msg)
{
    std::size_t pos = this->_readBuffer.find('\n');
    if (pos == std::string::npos)
    {
        if (this->_readBuffer.size() > 8192)
            this->_readBuffer.clear();
        return false;
    }

    std::size_t end = pos;
    if (end > 0 && this->_readBuffer[end - 1] == '\r')
        end--;

    msg = this->_readBuffer.substr(0, end);
    this->_readBuffer.erase(0, pos + 1);
    return true;
}

void               Client::queueMessage(const std::string& msg)
{
    this->_writeBuffer += msg;
}
const std::string& Client::getWriteBuffer() const
{
    return this->_writeBuffer;
}
void               Client::clearWriteBuffer(int sentBytes)
{
    this->_writeBuffer.erase(0, sentBytes);
}



int         Client::getFd() const
{
    return this->_fd;
}
std::string Client::getNickname() const
{
    return this->_nickname;
}
std::string Client::getUsername() const
{
    return this->_username;
}
std::string Client::getRealname() const
{
    return this->_realname;
}
std::string Client::getHostname() const
{
    return this->_hostname;
}

std::string Client::getPrefix() const
{
    return this->_nickname + "!" + this->_username + "@" + this->_hostname;
}


bool Client::isPassAccepted() const
{
    if (this->_passAccepted)
        return true;
    return false;
}
bool Client::isNickSet() const
{
    if (this->_nickSet)
        return true;
    return false;
}
bool Client::isUserSet() const
{
    if (this->_userSet)
        return true;
    return false;
}
bool Client::isFullyRegistered() const
{
    if(this->_passAccepted && this->_nickSet && this->_userSet)
        return true;
    return false;
}


void Client::setNickname(const std::string& nick)
{
    this->_nickname = nick;
}
void Client::setUsername(const std::string& user)
{
    this->_username = user;
}
void Client::setRealname(const std::string& real)
{
    this->_realname = real;
}
void Client::setHostname(const std::string& host)
{
    this->_hostname = host;
}
void Client::setPassAccepted(bool val)
{
    this->_passAccepted = val;
}
void Client::setNickSet(bool val)
{
    this->_nickSet = val;
}
void Client::setUserSet(bool val)
{
    this->_userSet = val;
}