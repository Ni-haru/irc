#ifndef CHANNEL_HPP
#define CHANNEL_HPP

#include <string>
#include <map>
#include <set>

class Client;

class Channel
{
public:
    Channel(const std::string& name);
    ~Channel();

    // ── getters ─────────────────────────────────
    const std::string&  getName()     const { return _name; }
    const std::string&  getTopic()    const { return _topic; }
    const std::string&  getKey()      const { return _key; }
    int                 getUserLimit() const { return _userLimit; }

    // ── mode flags ──────────────────────────────
    bool isInviteOnly()      const { return _inviteOnly; }
    bool isTopicRestricted() const { return _topicRestricted; }
    bool hasKey()            const { return !_key.empty(); }
    bool hasUserLimit()      const { return _userLimit > 0; }

    // ── setters (Person 3 fills logic) ──────────
    void setTopic(const std::string& topic)  { _topic = topic; }
    void setKey(const std::string& key)      { _key = key; }
    void removeKey()                         { _key.clear(); }
    void setUserLimit(int limit)             { _userLimit = limit; }
    void removeUserLimit()                   { _userLimit = -1; }
    void setInviteOnly(bool val)             { _inviteOnly = val; }
    void setTopicRestricted(bool val)        { _topicRestricted = val; }

    // ── membership ──────────────────────────────
    void    addMember(Client* client);
    void    removeMember(int fd);
    bool    isMember(int fd)   const;
    bool    isEmpty()          const { return _members.empty(); }
    int     getMemberCount()   const { return (int)_members.size(); }

    // ── operator management ─────────────────────
    void    addOperator(int fd);
    void    removeOperator(int fd);
    bool    isOperator(int fd) const;

    // ── invite list (mode +i) ───────────────────
    void    addInvited(int fd);
    bool    isInvited(int fd)  const;

    // ── broadcast ───────────────────────────────
    // sends msg to all members, optionally excluding one fd
    void    broadcast(const std::string& msg, int excludeFd = -1);

    // ── member list for NAMES reply ─────────────
    // returns "[@]nick [@]nick ..." with @ for operators
    std::string getMemberList() const;

    // ── members map (Person 3 may need direct access) ──
    const std::map<int, Client*>& getMembers() const { return _members; }

private:
    std::string             _name;
    std::string             _topic;
    std::string             _key;        // password (mode +k)
    int                     _userLimit;  // -1 = no limit (mode +l)

    bool                    _inviteOnly;       // mode +i
    bool                    _topicRestricted;  // mode +t

    std::map<int, Client*>  _members;    // fd → Client*
    std::set<int>           _operators;  // fds with op status
    std::set<int>           _invited;    // fds invited (mode +i)
};

#endif