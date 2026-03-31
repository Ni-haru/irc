#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

class client {
    private:
        int socketclient;
    public:
        client();
};
