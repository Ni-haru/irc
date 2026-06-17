#include "../includes/client.hpp"

client::client(std::string server_ip, int server_port, std::string nickname, std::string username, std::string pass) {
    this->socketclient = -1;
    this->server_ip = server_ip;
    this->server_port = server_port;
    this->nickname = nickname;
    this->username = username;
    this->pass = pass;
    this->recvmsg = "";
    this->fds[0].fd = 0;
    this->fds[0].events = POLLIN;
    this->fds[1].fd = socketclient;
    this->fds[1].events = POLLIN;
}

client::client(const client& other) {
    this->socketclient = other.socketclient;
    this->server_ip = other.server_ip;
    this->server_port = other.server_port;
    this->nickname = other.nickname;
    this->username = other.username;
    this->pass = other.pass;
}

client& client::operator=(const client& other) {
    if (this != &other) {
        this->socketclient = other.socketclient;
        this->server_ip = other.server_ip;
        this->server_port = other.server_port;
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
    sendMessage("NICK " + nickname);
}

void client::registerToserver() {
    sendMessage("PASS " + pass);
    sendMessage("NICK " + nickname);
    sendMessage("USER " + username + " 0 * :" + username);
}

void client::sendMessage(const std::string &message) {
    std::string msg = message + "\r\n";
    send(socketclient, msg.c_str(), msg.size(), 0);
}

void client::receiveMessage()
{
    char buffer[1024] = { 0 };
    ssize_t bytesRead = recv(socketclient, buffer, sizeof(buffer), 0);
    if (bytesRead == 0) {
        throw std::runtime_error("Connection closed by server");
    }
    if (bytesRead < 0) {
        throw std::runtime_error("Failed to receive message from server");
    }
    this->recvmsg += std::string(buffer, bytesRead);
    size_t pos;
    while ((pos = recvmsg.find("\r\n")) != std::string::npos)
    {
        std::string line = recvmsg.substr(0, pos);
        std::cout << "Message from server: " << line << std::endl;
        recvmsg.erase(0, pos + 2);
        handlePing(line);
        handleServerMessageError(line);
    }

}

void client::handlePing(const std::string &message) {
    if (message.size() > 4 && message.substr(0, 4) == "PING") {
        sendMessage("PONG " + message.substr(5));
    }
}

void client::handleServerMessageError(const std::string &message) {
    if (message.find("001 " + nickname) != std::string::npos) {
        std::cout << "Successfully registered to the server." << std::endl;
    } else if (message.find("433 " + nickname) != std::string::npos) {
        throw std::runtime_error("Nickname is already in use.");
    } else if (message.find("464 " + nickname) != std::string::npos) {
        throw std::runtime_error("Password is incorrect.");
    } else if (message.find("ERROR") != std::string::npos) {
        throw std::runtime_error("Error from server: " + message);
    } else if (message.find("462 " + nickname) != std::string::npos) {
        throw std::runtime_error("Already registered.");
    }
}

void client::run() {
    this->fds[0].fd = 0;
    this->fds[0].events = POLLIN;
    this->fds[1].fd = socketclient;
    this->fds[1].events = POLLIN;
    while (true) {
        int ready = poll(fds, 2, -1);
        if (ready == -1) {
            throw std::runtime_error("Poll failed");
        }
        if (fds[0].revents & POLLIN) {
            std::string input;
            if (!std::getline(std::cin, input) || input == "exit") {
                break;
            }
            sendMessage(input);
        }
        if (fds[1].revents & POLLIN) {
            receiveMessage();
        }

    }
}
