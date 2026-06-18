#include "../includes/client.hpp"
#include <iostream>

int main(int argc, char* argv[]) {
    std::string str = "PRIVMSG #chan hello\r\nPRIVMSG #chan hello\r\n";
    std::size_t pos = str.find("\r\n");
    if(pos != std::string::npos)
    {
        std::cout << "\\r\\n found in " << pos << std::endl;
        std::string str2 = str.substr(0, pos);
        std::cout << str2 << std::endl;
        str.erase(0, pos + 2);
        pos = str.find("\r\n");
        if(pos != std::string::npos)
        {
            std::cout << "\\r\\n found in " << pos << std::endl;
            str2 = str.substr(0, pos);
            std::cout << str2 << std::endl;
            str.erase(0, pos + 2);
            pos = str.find("\r\n");
            if(pos != std::string::npos)
            {
                std::cout << "\\r\\n found in " << pos << std::endl;
            }
            else {
                std::cout << "not found" << std::endl;
            }
        }
        else {
            std::cout << "not found" << std::endl;
        }
    }
    else {
        std::cout << "not found" << std::endl;
    }

    return 0;
}