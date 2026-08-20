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
    const int RPL_UMODEIS       = 221;
    const int RPL_ENDOFWHO      = 315;
    const int RPL_WHOISUSER     = 311;
    const int RPL_WHOISSERVER   = 312;
    const int RPL_ENDOFWHOIS    = 318;
    const int RPL_WHOISCHANNELS = 319;
    const int RPL_LISTSTART     = 321;
    const int RPL_LIST          = 322;
    const int RPL_LISTEND       = 323;
    const int RPL_CHANNELMODEIS = 324;
    const int RPL_CREATIONTIME  = 329;
    const int RPL_NOTOPIC       = 331;
    const int RPL_TOPIC         = 332;
    const int RPL_INVITING      = 341;
    const int RPL_WHOREPLY      = 352;
    const int RPL_NAMREPLY      = 353;
    const int RPL_ENDOFNAMES    = 366;

    // errors
    const int ERR_NOSUCHNICK       = 401;
    const int ERR_NOSUCHCHANNEL    = 403;
    const int ERR_CANNOTSENDTOCHAN = 404;
    const int ERR_NOORIGIN         = 409;
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
    const int ERR_UNKNOWNMODE      = 472;
    const int ERR_INVITEONLYCHAN   = 473;
    const int ERR_BADCHANNELKEY    = 475;
    const int ERR_CHANOPRIVSNEEDED = 482;
    const int ERR_USERSDONTMATCH   = 502;

    // ─────────────────────────────────────────
    // RFC 1459 "casemapping".
    // IRC treats nicknames and channel names case-insensitively, and the
    // standard goes one step further than plain ASCII: because the protocol
    // grew out of Scandinavian character sets, {}|^ are defined as the
    // lowercase forms of []\~. Every place that *compares* or *looks up* a
    // nick or a channel goes through this function, while the string we
    // display back to users keeps the exact casing they typed.
    // ─────────────────────────────────────────
    inline std::string toLower(const std::string& s)
    {
        std::string out = s;
        for (std::size_t i = 0; i < out.size(); i++)
        {
            char c = out[i];
            if (c >= 'A' && c <= 'Z')  c = static_cast<char>(c - 'A' + 'a');
            else if (c == '[')         c = '{';
            else if (c == ']')         c = '}';
            else if (c == '\\')        c = '|';
            else if (c == '~')         c = '^';
            out[i] = c;
        }
        return out;
    }

    // Same idea applied to a command verb: commands are pure ASCII and are
    // case-insensitive, so "ping", "PiNg" and "PING" are one command.
    inline std::string toUpper(const std::string& s)
    {
        std::string out = s;
        for (std::size_t i = 0; i < out.size(); i++)
        {
            if (out[i] >= 'a' && out[i] <= 'z')
                out[i] = static_cast<char>(out[i] - 'a' + 'A');
        }
        return out;
    }

    // format: ":serverName CODE target :message\r\n"
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

    // Same, but without the trailing " :" part. Numerics such as
    // 324 RPL_CHANNELMODEIS end with real arguments (the mode string), not
    // with a free-text message, so they must not carry a colon there.
    inline std::string makeRawReply(int code,
                                     const std::string& rest,
                                     const std::string& serverName = "ircserv")
    {
        std::ostringstream ss;
        ss << ":" << serverName << " ";
        if (code < 100) ss << "0";
        if (code < 10)  ss << "0";
        ss << code << " " << rest << "\r\n";
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