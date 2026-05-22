#ifndef CLIENT_HPP
#define CLIENT_HPP

#include <string>
#include <unistd.h>  
#include <sys/socket.h>

class Client
{
public:
    Client(int fd);
    ~Client();

    int                 getFd()       const { return _fd; }
    const std::string&  getNickname() const { return _nickname; }
    const std::string&  getUsername() const { return _username; }
    bool                isRegistered() const { return _registered; }

    // called by Server when poll() says POLLIN
    void        appendToBuffer(const std::string& data);
    // returns true and fills 'msg' if a complete \r\n message is ready
    bool        getNextMessage(std::string& msg);

    // queue a message to send; Server flushes it on POLLOUT
    void        queueMessage(const std::string& msg);
    // returns data waiting to be sent
    const std::string& getWriteBuffer() const { return _writeBuffer; }
    void        clearWriteBuffer(size_t bytes) { _writeBuffer.erase(0, bytes); }

    // Person 2 will add: setNickname, setUsername, authenticate(), etc.
    void        setNickname(const std::string& nick) { _nickname = nick; }
    void        setUsername(const std::string& user) { _username = user; }
    void        setRegistered(bool r)                { _registered = r; }

private:
    int         _fd;
    std::string _nickname;
    std::string _username;
    bool        _authenticated; // PASS done
    bool        _registered;    // NICK + USER done

    std::string _readBuffer;    // raw bytes received, waiting for \r\n
    std::string _writeBuffer;   // messages queued to send
};

#endif