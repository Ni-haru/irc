#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

class client {
    private:
        int socketclient;
        std::string server_ip;
        int server_port;
        std::string message;
        std::string nickname;
        std::string username;
        std::string pass;
    public:
        client(std::string server_ip, int server_port, std::string nickname, std::string username, std::string pass);
        client (const client& other);
        client& operator=(const client& other);
        ~client();

        void connectToServer();
        void sendMessage();
};
