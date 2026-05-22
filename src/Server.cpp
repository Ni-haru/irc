#include "Server.hpp"

bool Server::_stop = false;


Server::Server(int port, const std::string& password)
    : _port(port), _password(password), _serverFd(-1)
{
    _createSocket();
    _setSocketOptions();
    _setNonBlocking(_serverFd);
    _bindSocket();
    _listenSocket();

    pollfd pfd;
    pfd.fd      = _serverFd;
    pfd.events  = POLLIN;   
    pfd.revents = 0;
    _fds.push_back(pfd);

    signal(SIGINT,  Server::_signalHandler);
    signal(SIGTERM, Server::_signalHandler);

    std::cout << "Server listening on port " << _port << std::endl;
}

Server::~Server()
{
    for (std::map<int, Client*>::iterator it = _clients.begin();
         it != _clients.end(); ++it)
    {
        close(it->first);
        delete it->second;
    }
    for (std::map<std::string, Channel*>::iterator it = _channels.begin();
         it != _channels.end(); ++it)
        delete it->second;

    if (_serverFd != -1)
        close(_serverFd);

    std::cout << "Server shut down." << std::endl;
}


void Server::_signalHandler(int sig)
{
    (void)sig;
    std::cout << "\nShutting down..." << std::endl;
    Server::_stop = true;
}

void Server::run()
{
    while (!Server::_stop)
    {
        int ready = poll(_fds.data(), _fds.size(), -1);

        if (ready == -1)
        {
            if (Server::_stop) break;  
            std::cerr << "poll() error" << std::endl;
            break;
        }

        if (_fds[0].revents & POLLIN)
            _acceptClient();
        for (size_t i = 1; i < _fds.size(); i++)
        {
            int fd = _fds[i].fd;

            if (_fds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
            {
                _disconnectClient(fd);
                i--;
                continue;
            }
            if (_fds[i].revents & POLLIN)
                _readFromClient(fd);

            if (_fds[i].revents & POLLOUT)
                _flushWriteBuffer(fd);
        }
    }
}

void Server::_acceptClient()
{
    struct sockaddr_in clientAddr;
    socklen_t          addrLen = sizeof(clientAddr);

    int clientFd = accept(_serverFd, (struct sockaddr*)&clientAddr, &addrLen);
    if (clientFd == -1)
    {
        std::cerr << "accept() failed" << std::endl;
        return;
    }
    _setNonBlocking(clientFd);
    pollfd pfd;
    pfd.fd      = clientFd;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    _fds.push_back(pfd);

    _clients[clientFd] = new Client(clientFd);

    std::cout << "New client connected: fd=" << clientFd << std::endl;
}

void Server::_readFromClient(int fd)
{
    char    buf[512];
    int     bytes = recv(fd, buf, sizeof(buf) - 1, 0);

    if (bytes <= 0)
    {
        _disconnectClient(fd);
        return;
    }

    buf[bytes] = '\0';
    _clients[fd]->appendToBuffer(std::string(buf, bytes));
    std::string msg;
    while (_clients[fd]->getNextMessage(msg))
        _handleMessage(fd, msg);
}

void Server::_flushWriteBuffer(int fd)
{
    Client* client = _clients[fd];
    const std::string& buf = client->getWriteBuffer();

    if (buf.empty())
    {
        for (size_t i = 0; i < _fds.size(); i++)
        {
            if (_fds[i].fd == fd)
            {
                _fds[i].events &= ~POLLOUT;
                break;
            }
        }
        return;
    }

    int sent = send(fd, buf.c_str(), buf.size(), 0);
    if (sent > 0)
        client->clearWriteBuffer(sent);
    else if (sent == -1)
        _disconnectClient(fd);
}

void Server::_disconnectClient(int fd)
{
    std::cout << "Client disconnected: fd=" << fd << std::endl;

    for (size_t i = 0; i < _fds.size(); i++)
    {
        if (_fds[i].fd == fd)
        {
            _fds.erase(_fds.begin() + i);
            break;
        }
    }


    if (_clients.count(fd))
    {
        delete _clients[fd];
        _clients.erase(fd);
    }

    close(fd);
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

IRCMessage Server::_parseMessage(const std::string& raw)
{
    IRCMessage msg;
    std::string line = raw;
    size_t pos = 0;

    if (!line.empty() && line[0] == ':')
    {
        pos = line.find(' ');
        if (pos == std::string::npos)
            return msg;
        msg.prefix = line.substr(1, pos - 1); 
        line = line.substr(pos + 1);
    }
    pos = line.find(' ');
    if (pos == std::string::npos)
    {
        msg.command = line;  
        return msg;
    }
    msg.command = line.substr(0, pos);
    line = line.substr(pos + 1);
 
    while (!line.empty())
    {
        if (line[0] == ':')
        {
            msg.params.push_back(line.substr(1));
            break;
        }
        pos = line.find(' ');
        if (pos == std::string::npos)
        {
            msg.params.push_back(line);
            break;
        }
        msg.params.push_back(line.substr(0, pos));
        line = line.substr(pos + 1);
    }
 
    return msg;
}
 


void Server::sendToAll(const std::string& msg)
{
    for (std::map<int, Client*>::iterator it = _clients.begin();
         it != _clients.end(); ++it)
        sendToClient(it->first, msg);
}

void Server::handlePING(int fd, IRCMessage& msg)
{
    std::string token = msg.params.empty() ? "ircserv" : msg.params[0];
    sendToClient(fd, "PONG :ircserv " + token + "\r\n");
}


void Server::_handleMessage(int fd, const std::string& raw)
{
    if (raw.empty()) return;
 
    IRCMessage msg = _parseMessage(raw);
    if (msg.command.empty()) return;
 
    std::cout << "fd=" << fd
              << " cmd=" << msg.command
              << " params=" << msg.params.size()
              << std::endl;
 
    // ── dispatch table ───────────────────────
    // Your job: wire up the command string to the handler.
    // Person 2 implements: PASS, NICK, USER, QUIT, PART, PRIVMSG, PING
    // Person 3 implements: JOIN, KICK, INVITE, TOPIC, MODE
 
    if      (msg.command == "PASS")    handlePASS(fd, msg);
    else if (msg.command == "NICK")    handleNICK(fd, msg);
    else if (msg.command == "USER")    handleUSER(fd, msg);
    else if (msg.command == "PING")    handlePING(fd, msg);
    else if (msg.command == "QUIT")    handleQUIT(fd, msg);
    else if (msg.command == "PART")    handlePART(fd, msg);
    else if (msg.command == "PRIVMSG") handlePRIVMSG(fd, msg);
    else if (msg.command == "JOIN")    handleJOIN(fd, msg);
    else if (msg.command == "KICK")    handleKICK(fd, msg);
    else if (msg.command == "INVITE")  handleINVITE(fd, msg);
    else if (msg.command == "TOPIC")   handleTOPIC(fd, msg);
    else if (msg.command == "MODE")    handleMODE(fd, msg);
    else
    {
        if (_clients.count(fd))
        {
            std::string nick = _clients[fd]->getNickname();
            if (nick.empty()) nick = "*";
            sendToClient(fd, IRC::makeReply(IRC::ERR_UNKNOWNCOMMAND,
                nick, msg.command + " :Unknown command"));
        }
    }
}

void Server::sendToClient(int fd, const std::string& msg)
{
    if (!_clients.count(fd)) return;
 
    _clients[fd]->queueMessage(msg);
 
    for (size_t i = 0; i < _fds.size(); i++)
    {
        if (_fds[i].fd == fd)
        {
            _fds[i].events |= POLLOUT;
            break;
        }
    }
}

