#include "../includes/client.hpp"

Client::Client(int fd){
    this->_fd = fd;
}


void    Client::appendToBuffer(const std::string& data){
    this->_readBuffer += data;
}

bool    Client::getNextMessage(std::string& msg)
{
    std::size_t pos = this->_readBuffer.find("\r\n");
    if(pos == std::string::npos){
        return false;
    }
    msg = this->_readBuffer.substr(0, pos);
    this->_readBuffer.erase(0, pos + 2);
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
