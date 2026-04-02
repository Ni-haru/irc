#include "../includes/client.hpp"

client::client(std::string server_ip, int server_port, std::string nickname, std::string username, std::string pass) {
    this->socketclient = -1;
    this->server_ip = server_ip;
    this->server_port = server_port;
    this->nickname = nickname;
    this->username = username;
    this->pass = pass;
    this->message = "NICK " + nickname + "\r\n" + "USER " + username + " 0 * :" + username + "\r\n" + "PASS " + pass;
}

client::client(const client& other) {
    this->socketclient = other.socketclient;
    this->server_ip = other.server_ip;
    this->server_port = other.server_port;
    this->message = other.message;
    this->nickname = other.nickname;
    this->username = other.username;
    this->pass = other.pass;
}

client& client::operator=(const client& other) {
    if (this != &other) {
        this->socketclient = other.socketclient;
        this->server_ip = other.server_ip;
        this->server_port = other.server_port;
        this->message = other.message;
        this->nickname = other.nickname;
        this->username = other.username;
        this->pass = other.pass;
    }
    return *this;
}

client::~client() {
    if (socketclient != -1) {
        close(socketclient);
    }
}

void client::connectToServer() {
    socketclient = socket(AF_INET, SOCK_STREAM, 0);
    if (socketclient < -1)
        throw std::runtime_error("Failed to create socket");
    sockaddr_in serverAddress;
    std::memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip.c_str(), &serverAddress.sin_addr) <= 0)
        throw std::runtime_error("Invalid server IP address");

    if (connect(socketclient, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) == -1)
        throw std::runtime_error("Failed to connect to server");
}

void client::sendMessage() {
    message += "\r\n";
    send(socketclient, message.c_str(), message.size(), 0);
}
