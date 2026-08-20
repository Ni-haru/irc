#ifndef IRCMESSAGE_HPP
#define IRCMESSAGE_HPP

#include <string>
#include <vector>
#include <sstream>

// ─────────────────────────────────────────────
// Parsed IRC message
// raw:  ":alice!u@h PRIVMSG #chan :hello\r\n"
// becomes:
//   prefix  = "alice!u@h"
//   command = "PRIVMSG"
//   params  = ["#chan", "hello"]
// ─────────────────────────────────────────────
struct IRCMessage
{
    std::string              prefix;
    std::string              command;
    std::vector<std::string> params;
};


namespace IRC
{
    // success
    const int RPL_WELCOME       = 1;
    const int RPL_YOURHOST      = 2;
    const int RPL_CREATED       = 3;
    const int RPL_MYINFO        = 4;
    const int RPL_NAMREPLY      = 353;
    const int RPL_ENDOFNAMES    = 366;
    const int RPL_NOTOPIC       = 331;
    const int RPL_TOPIC         = 332;
    const int RPL_INVITING      = 341;

    // errors
    const int ERR_NOSUCHNICK       = 401;
    const int ERR_NOSUCHCHANNEL    = 403;
    const int ERR_CANNOTSENDTOCHAN = 404;
    const int ERR_NORECIPIENT      = 411;
    const int ERR_NOTEXTTOSEND     = 412;
    const int ERR_UNKNOWNCOMMAND   = 421;
    const int ERR_NONICKNAMEGIVEN  = 431;
    const int ERR_ERRONEUSNICK     = 432;
    const int ERR_NICKNAMEINUSE    = 433;
    const int ERR_USERNOTINCHANNEL = 441;
    const int ERR_NOTONCHANNEL     = 442;
    const int ERR_USERONCHANNEL    = 443;
    const int ERR_NOTREGISTERED    = 451;
    const int ERR_NEEDMOREPARAMS   = 461;
    const int ERR_ALREADYREGISTRED = 462;
    const int ERR_PASSWDMISMATCH   = 464;
    const int ERR_CHANNELISFULL    = 471;
    const int ERR_INVITEONLYCHAN   = 473;
    const int ERR_BADCHANNELKEY    = 475;
    const int ERR_CHANOPRIVSNEEDED = 482;

    // format: ":serverName CODE targetNick :message\r\n"
    inline std::string makeReply(int code,
                                  const std::string& target,
                                  const std::string& msg,
                                  const std::string& serverName = "ircserv")
    {
        std::ostringstream ss;
        ss << ":" << serverName << " ";
        if (code < 100) ss << "0";
        if (code < 10)  ss << "0";
        ss << code << " " << target << " :" << msg << "\r\n";
        return ss.str();
    }

    // format: ":nick!user@host COMMAND params\r\n"
    inline std::string makeMsg(const std::string& prefix,
                                const std::string& command,
                                const std::string& params)
    {
        return ":" + prefix + " " + command + " " + params + "\r\n";
    }
}

#endif