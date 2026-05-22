#include "Client.hpp"
#include <iostream>

Client::Client(int fd)
    : _fd(fd), _authenticated(false), _registered(false)
{}

Client::~Client()
{
    // fd is closed by Server — don't close here
}

// ─────────────────────────────────────────────
// appendToBuffer
//
// Called every time poll() says POLLIN for this client.
// We don't process yet — we just accumulate raw bytes.
// TCP can deliver "HEL" in one call and "LO\r\n" in the next.
// ─────────────────────────────────────────────
void Client::appendToBuffer(const std::string& data)
{
    _readBuffer += data;
}

// ─────────────────────────────────────────────
// getNextMessage
//
// Scans the buffer for the IRC message delimiter \r\n.
// If found: extracts that message, removes it from the
// buffer, stores it in 'msg', returns true.
// If not found yet: returns false (more data needed).
//
// The caller loops on this until it returns false:
//   while (client->getNextMessage(msg))
//       handleMessage(fd, msg);
// ─────────────────────────────────────────────
bool Client::getNextMessage(std::string& msg)
{
    size_t pos = _readBuffer.find("\r\n");
    if (pos == std::string::npos)
        return false;

    msg = _readBuffer.substr(0, pos);   // message without \r\n
    _readBuffer.erase(0, pos + 2);      // remove message + \r\n from buffer
    return true;
}

// ─────────────────────────────────────────────
// queueMessage
//
// Person 2 calls this when they want to send something.
// We don't send immediately — we add to _writeBuffer.
// Server flushes it when poll() says POLLOUT.
// ─────────────────────────────────────────────
void Client::queueMessage(const std::string& msg)
{
    _writeBuffer += msg;
}