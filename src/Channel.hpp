#ifndef CHANNEL_HPP
#define CHANNEL_HPP

#include <string>
#include <vector>
#include <sys/socket.h>

class Client;

class Channel
{
    private:
        std::string _name;
        std::vector<Client*> _clients;
        std::vector<Client*> _operators;
        std::string _topic;
        std::vector<Client*> _invitedClients;

        bool _inviteOnly;
        bool _topicRestricted;
        std::string _key;
        int _userLimit;

    public:
        Channel(const std::string& name);
        ~Channel();
        bool hasClient(Client* client) const;
        void addClient(Client* client);
        void removeClient(Client* client);
        void addOperator(Client* client);
        void removeOperator(Client* client);
        bool isOperator(Client* client) const;
        bool empty() const;
        const std::string& getTopic() const;
        void setTopic(const std::string& topic);
        void broadcast(const std::string& msg, Client* sender);
        const std::vector<Client*>& getClients() const;
        void addInvited(Client* client);
        void removeInvited(Client* client);
        bool isInvited(Client* client) const;

        // --
        bool isInviteOnly() const;
        void setInviteOnly(bool value);

        bool isTopicRestricted() const;
        void setTopicRestricted(bool value);

        bool hasKey() const;
        void setKey(const std::string& key);
        bool checkKey(const std::string& key) const;

        int  getUserLimit() const;
        void setUserLimit(int limit);
        bool isFull() const;
        bool hasLimit() const;

        const std::string& getName() const;
};


#endif
