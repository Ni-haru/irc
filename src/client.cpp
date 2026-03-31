#include "../includes/client.hpp"

client::client() {
    {
            socketclient = socket(AF_INET, SOCK_STREAM, 0);
            if (socketclient == -1) {
                std::cerr << "Failed to create socket" << std::endl;
                exit(1);
            }
            sockaddr_in serverAddress;
serverAddress.sin_family = AF_INET;
serverAddress.sin_port = htons(8080);
serverAddress.sin_addr.s_addr = INADDR_ANY;
connect(socketclient, (struct sockaddr*)&serverAddress, sizeof(serverAddress));
const char* message = "Hello, server!";
send(socketclient, message, strlen(message), 0);
close(socketclient);
        }
}