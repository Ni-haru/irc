#ifndef CLIENT_HPP
#define CLIENT_HPP

#include <string>

class Client
{
private:
    int         _fd;
    std::string _nickname;
    std::string _username;
    std::string _realname;
    std::string _hostname;

    std::string _readBuffer;
    std::string _writeBuffer;

    bool        _passAccepted;
    bool        _nickSet;
    bool        _userSet;

public:
    Client(int fd);
    ~Client();

    void        appendToBuffer(const std::string& data);
    bool        getNextMessage(std::string& msg);

    void               queueMessage(const std::string& msg);
    const std::string& getWriteBuffer() const;
    void               clearWriteBuffer(int sentBytes);

    int         getFd() const;
    std::string getNickname() const;
    std::string getUsername() const;
    std::string getRealname() const;
    std::string getHostname() const;

    bool isPassAccepted() const;
    bool isNickSet() const;
    bool isUserSet() const;
    bool isFullyRegistered() const;

    void setNickname(const std::string& nick);
    void setUsername(const std::string& user);
    void setRealname(const std::string& real);
    void setHostname(const std::string& host);
    void setPassAccepted(bool val);
    void setNickSet(bool val);
    void setUserSet(bool val);

    std::string getPrefix() const;
};

#endif