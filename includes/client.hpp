#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <poll.h>

class client {
    private:
        int socketclient;
        std::string server_ip;
        int server_port;
        std::string nickname;
        std::string username;
        std::string pass;
        std::string recvmsg;
        pollfd fds[2];
    public:
        client(std::string server_ip, int server_port, std::string nickname, std::string username, std::string pass);
        client (const client& other);
        client& operator=(const client& other);
        ~client();

        void connectToServer();
        void registerToserver();
        void sendMessage(const std::string &message);
        void receiveMessage();
        void handlePing(const std::string &message);
        void handleServerMessageError(const std::string &message);
        void run();
};
