#ifndef SERVER_HPP
#define SERVER_HPP

#include <string>
#include <stdexcept>


#include <sys/socket.h>   
#include <netinet/in.h>  
#include <fcntl.h>        
#include <unistd.h>       

class Server
{
public:
    Server(int port, const std::string& password);
    ~Server();

    void run();           
private:
    int         _port;
    std::string _password;
    int         _serverFd;  

   
    void _createSocket();
    void _setSocketOptions();
    void _setNonBlocking(int fd);
    void _bindSocket();
    void _listenSocket();
};

#endif