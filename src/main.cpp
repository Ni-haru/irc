#include "Server.hpp"
#include <iostream>
#include <cstdlib>  
#include <cctype>    

static bool isValidPortStr(const std::string& str)
{
    if (str.empty())
        return false;
    for (size_t i = 0; i < str.size(); i++)
    {
        if (!std::isdigit(str[i]))
            return false;
    }
    return true;
}

int main(int argc, char* argv[])
{

    if (argc != 3)
    {
        std::cerr << "Usage: ./ircserv <port> <password>" << std::endl;
        return 1;
    }

    std::string portStr  = argv[1];
    std::string password = argv[2];

  
    if (!isValidPortStr(portStr))
    {
        std::cerr << "Error: port must be a number." << std::endl;
        return 1;
    }

 
    int port = std::atoi(portStr.c_str());
    if (port < 1024 || port > 65535)
    {
        std::cerr << "Error: port must be between 1024 and 65535." << std::endl;
        return 1;
    }

    if (password.empty())
    {
        std::cerr << "Error: password cannot be empty." << std::endl;
        return 1;
    }

   
    try
    {
        Server server(port, password);
        server.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}