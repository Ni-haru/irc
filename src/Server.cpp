#include "Server.hpp"
#include <iostream>
#include <cstring>      


Server::Server(int port, const std::string& password)
    : _port(port), _password(password), _serverFd(-1)
{
    _createSocket();        // 1. socket()
    _setSocketOptions();    // 2. setsockopt()
    _setNonBlocking(_serverFd); // 3. fcntl()
    _bindSocket();          // 4. bind()
    _listenSocket();        // 5. listen()

    std::cout << "Server listening on port " << _port << std::endl;
}

Server::~Server()
{
    if (_serverFd != -1)
        close(_serverFd);
}

void Server::run()
{
    std::cout << "Server running... (poll loop not yet implemented)" << std::endl;
}

void Server::_createSocket()
{
    _serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (_serverFd == -1)
        throw std::runtime_error("socket() failed");
}


void Server::_setSocketOptions()
{
    int opt = 1;  
    if (setsockopt(_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        close(_serverFd);
        throw std::runtime_error("setsockopt() failed");
    }
}

void Server::_setNonBlocking(int fd)
{
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1)
    {
        close(_serverFd);
        throw std::runtime_error("fcntl() failed");
    }
}

void Server::_bindSocket()
{
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));

    addr.sin_family      = AF_INET;          
    addr.sin_port        = htons(_port);     
    addr.sin_addr.s_addr = INADDR_ANY;      

    if (bind(_serverFd, (struct sockaddr*)&addr, sizeof(addr)) == -1)
    {
        close(_serverFd);
        throw std::runtime_error("bind() failed: port may already be in use");
    }
}


void Server::_listenSocket()
{
    if (listen(_serverFd, 10) == -1)
    {
        close(_serverFd);
        throw std::runtime_error("listen() failed");
    }
}