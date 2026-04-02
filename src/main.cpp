#include "../includes/client.hpp"

int main(int argc, char* argv[]) {
    if (argc != 6) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <server_port> <pass> <nickname> <username>" << std::endl;
        return 1;
    }
    try
    {
        client myClient(argv[1], std::atoi(argv[2]), argv[4], argv[5], argv[3]);
        myClient.connectToServer();
        myClient.sendMessage();
    }
    catch(const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    

    return 0;
}