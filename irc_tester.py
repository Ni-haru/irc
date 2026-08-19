#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ft_irc tester - mirrors the 42 evaluation sheet.

usage:
    python3 irc_tester.py                    # builds ./ircserv, runs everything
    python3 irc_tester.py --binary ./ircserv --port 6667 --password pass
    python3 irc_tester.py --only channel     # run one section
    python3 irc_tester.py --no-static        # skip source grep checks

Every test group runs against a FRESH server process, so one crash does not
hide the rest of the results. A crash is reported as CRASH (that is a 0 at
defense), a wrong reply as FAIL.
"""

import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import time

G = "\033[32m"; R = "\033[31m"; Y = "\033[33m"; B = "\033[36m"; D = "\033[2m"; N = "\033[0m"

results = []          # (section, name, status, detail)
CUR_SECTION = "misc"


def record(name, status, detail=""):
    results.append((CUR_SECTION, name, status, detail))
    color = {"PASS": G, "FAIL": R, "CRASH": R, "WARN": Y, "SKIP": D}[status]
    line = f"  {color}[{status:5}]{N} {name}"
    if detail:
        line += f"\n          {D}{detail}{N}"
    print(line)


def section(title):
    global CUR_SECTION
    CUR_SECTION = title
    print(f"\n{B}=== {title} ==={N}")


# ───────────────────────── server process ─────────────────────────

class Server:
    def __init__(self, binary, port, password):
        self.binary, self.port, self.password = binary, port, password
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [self.binary, str(self.port), self.password],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        # wait for the port to actually accept
        for _ in range(50):
            time.sleep(0.05)
            if self.proc.poll() is not None:
                return False
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.2).close()
                return True
            except OSError:
                continue
        return False

    def death(self):
        """None if alive, else a human string describing how it died."""
        rc = self.proc.poll()
        if rc is None:
            return None
        if rc < 0:
            try:
                nm = signal.Signals(-rc).name
            except ValueError:
                nm = str(-rc)
            return f"killed by {nm}"
        err = b""
        try:
            err = self.proc.stderr.read() or b""
        except Exception:
            pass
        msg = err.decode(errors="replace").strip().splitlines()
        return f"exited with code {rc}" + (f' - "{msg[-1]}"' if msg else "")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
        if self.proc:
            try:
                self.proc.communicate(timeout=2)
            except Exception:
                pass


# ───────────────────────── client helper ─────────────────────────

class C:
    """One IRC connection."""

    def __init__(self, port, name="c"):
        self.name = name
        self.buf = ""
        self.s = socket.create_connection(("127.0.0.1", port), 3)
        self.s.settimeout(0.2)

    def send(self, line, crlf=True):
        self.s.sendall((line + ("\r\n" if crlf else "")).encode())
        return self

    def raw(self, data):
        self.s.sendall(data if isinstance(data, bytes) else data.encode())
        return self

    def read(self, seconds=0.6):
        """Drain everything that arrives within `seconds`."""
        end = time.time() + seconds
        while time.time() < end:
            try:
                chunk = self.s.recv(8192)
                if not chunk:
                    break
                self.buf += chunk.decode(errors="replace")
                end = time.time() + 0.15   # keep reading while data flows
            except socket.timeout:
                continue
            except OSError:
                break
        out, self.buf = self.buf, ""
        return out

    def expect(self, out, *needles):
        return all(n in out for n in needles)

    def register(self, nick, password, user=None):
        user = user or nick
        self.send(f"PASS {password}")
        self.send(f"NICK {nick}")
        self.send(f"USER {user} 0 * :Real {nick}")
        return self.read(0.8)

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def kill_hard(self):
        """RST instead of FIN - simulates an abruptly killed client."""
        try:
            import struct
            self.s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                              struct.pack("ii", 1, 0))
            self.s.close()
        except Exception:
            self.close()


# ───────────────────────── test runner ─────────────────────────

def group(srv, name, fn):
    """Run fn(port) against a fresh server; catch crashes."""
    if not srv.start():
        record(name, "CRASH", "server would not start / bind " + (srv.death() or ""))
        return
    try:
        fn(srv)
    except (ConnectionResetError, BrokenPipeError, ConnectionRefusedError, socket.timeout) as e:
        d = srv.death()
        if d:
            record(name + " (connection lost)", "CRASH", d)
        else:
            record(name + " (connection lost)", "FAIL", repr(e))
    except AssertionError as e:
        record(name, "FAIL", str(e))
    except Exception as e:
        record(name, "FAIL", f"tester error: {type(e).__name__}: {e}")
    finally:
        d = srv.death()
        if d:
            record(f"server survived: {name}", "CRASH", d)
        srv.stop()


def check(cond, name, detail=""):
    record(name, "PASS" if cond else "FAIL", "" if cond else detail)
    return cond


# ───────────────────────── 1. static checks ─────────────────────────

def static_checks(srcdir):
    section("Basic checks (static / source)")
    files = []
    for root, _, names in os.walk(srcdir):
        if ".git" in root:
            continue
        for n in names:
            if n.endswith((".cpp", ".hpp", ".h", ".tpp", ".ipp")):
                files.append(os.path.join(root, n))
    if not files:
        record("source files found", "SKIP", f"nothing under {srcdir}")
        return
    text = ""
    for f in files:
        try:
            text += open(f, errors="replace").read() + "\n"
        except Exception:
            pass
    code = re.sub(r"//[^\n]*", "", text)
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    code = re.sub(r'"(\\.|[^"\\])*"', '""', code)   # drop string literals

    polls = len(re.findall(r"\b(poll|select|kqueue|kevent|epoll_wait)\s*\(", code))
    check(polls == 1, f"exactly one poll()/equivalent call site ({polls} found)",
          "the eval sheet is explicit: more than one is an instant 0")

    bad_fcntl = [m for m in re.findall(r"fcntl\s*\([^;]*\)", code)
                 if not re.search(r"F_SETFL\s*,\s*O_NONBLOCK", m)]
    check(not bad_fcntl, "fcntl used only as fcntl(fd, F_SETFL, O_NONBLOCK)",
          "offending: " + "; ".join(bad_fcntl[:3]))

    check(not re.search(r"\bfork\s*\(", code), "no fork()")
    check(not re.search(r"\b(pthread_create|std::thread)\b", code), "no threads")

    errno_use = re.findall(r"errno\s*[=!]=\s*(EAGAIN|EWOULDBLOCK|EINTR)", code)
    check(not errno_use, "no errno-driven retry after recv/send",
          f"found {len(errno_use)} comparison(s) - the sheet forbids using errno to "
          "trigger an action after read/recv/write/send")

    check(bool(re.search(r"\bstd::(unordered_map|shared_ptr|auto\b|nullptr)", code)) is False,
          "no obvious C++11 constructs")


# ───────────────────────── 2. networking ─────────────────────────

def t_listen_and_answer(srv):
    a = C(srv.port, "a")
    out = a.register("alice", srv.password)
    check("001" in out, "registration returns RPL_WELCOME (001)", repr(out[:160]))
    check("004" in out, "registration returns 001-004 block", repr(out[:200]))
    a.close()


def t_multi_clients(srv):
    cs = [C(srv.port, f"c{i}") for i in range(5)]
    for i, c in enumerate(cs):
        c.register(f"user{i}", srv.password)
    ok = True
    for i, c in enumerate(cs):
        c.send("PING :probe")
        if "PONG" not in c.read(0.5):
            ok = False
    check(ok, "5 simultaneous clients all get answered")
    for c in cs:
        c.close()


def t_ping(srv):
    a = C(srv.port); a.register("alice", srv.password)
    a.send("PING :hello"); out = a.read()
    check("PONG" in out and "hello" in out, "PING gets a PONG carrying the token", repr(out[:120]))
    a.close()


def t_channel_broadcast(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.send("JOIN #test"); out_a = a.read(0.8)
    check("JOIN" in out_a, "JOIN is echoed back to the joiner", repr(out_a[:200]))
    check("353" in out_a and "366" in out_a, "JOIN sends NAMES (353) + end of NAMES (366)",
          repr(out_a[:200]))
    b.send("JOIN #test"); b.read(0.8)
    out_a = a.read(0.6)
    check("bob" in out_a and "JOIN" in out_a, "existing member is told when someone joins",
          repr(out_a[:200]))
    a.send("PRIVMSG #test :hello channel")
    got = b.read(0.8)
    check("hello channel" in got, "channel PRIVMSG reaches the other member", repr(got[:200]))
    mine = a.read(0.3)
    check("hello channel" not in mine, "sender does NOT get an echo of its own channel message")
    a.close(); b.close()


def t_privmsg_direct(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.read(); b.read()
    a.send("PRIVMSG bob :private hi")
    got = b.read(0.8)
    check("private hi" in got, "direct PRIVMSG nick reaches the target", repr(got[:200]))
    a.send("PRIVMSG ghost :nobody")
    got = a.read(0.6)
    check("401" in got, "PRIVMSG to unknown nick returns 401", repr(got[:160]))
    a.send("PRIVMSG")
    got = a.read(0.6)
    check("411" in got or "461" in got, "PRIVMSG with no target returns 411/461", repr(got[:160]))
    a.send("PRIVMSG bob")
    got = a.read(0.6)
    check("412" in got or "461" in got, "PRIVMSG with no text returns 412/461", repr(got[:160]))
    a.close(); b.close()


# ───────────────────────── 3. networking specials ─────────────────────────

def t_partial_command(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    b = C(srv.port); b.register("bob", srv.password); b.read()
    for piece in ["PI", "NG :sp", "lit"]:
        a.raw(piece); time.sleep(0.15)
    got = a.read(0.3)
    check("PONG" not in got, "partial command is buffered, not executed early", repr(got[:120]))
    a.raw("\r\n")
    got = a.read(0.8)
    check("PONG" in got and "split" in got, "partial command completes correctly once CRLF arrives",
          repr(got[:160]))
    b.send("PING :other"); got = b.read(0.6)
    check("PONG" in got, "other clients keep working while one has a partial command pending")
    a.close(); b.close()


def t_multiple_commands_one_packet(srv):
    a = C(srv.port)
    a.raw(f"PASS {srv.password}\r\nNICK alice\r\nUSER alice 0 * :A\r\nPING :x\r\n")
    got = a.read(1.0)
    check("001" in got and "PONG" in got,
          "several commands in one TCP packet are all processed", repr(got[:200]))
    a.close()


def t_bare_lf(srv):
    a = C(srv.port)
    a.raw(f"PASS {srv.password}\nNICK lf\nUSER lf 0 * :L\n")
    got = a.read(1.0)
    record("accepts bare LF line endings (plain nc, no -C)",
           "PASS" if "001" in got else "WARN",
           "" if "001" in got else "server only splits on CRLF; a grader using plain "
                                   "`nc` (without -C) will see a dead server")
    a.close()


def t_abrupt_kill(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.send("JOIN #room"); b.send("JOIN #room"); a.read(); b.read()
    a.kill_hard(); time.sleep(0.4)
    if srv.death():
        return  # group() reports the crash
    b.send("PING :still-alive"); got = b.read(0.8)
    check("PONG" in got, "server survives a client killed abruptly (RST)")
    c = C(srv.port); out = c.register("carol", srv.password)
    check("001" in out, "new clients can still connect after an abrupt kill")
    c.send("JOIN #room"); got = c.read(0.8)
    check("353" in got, "joining a channel a dead client was in still works "
                        "(dangling Client* check)", repr(got[:200]))
    b.send("PRIVMSG #room :after death"); got = c.read(0.8)
    check("after death" in got, "broadcasting to a channel a dead client was in works",
          repr(got[:200]))
    b.close(); c.close()


def t_half_command_kill(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.raw("PRIVMSG #room :half of a comm")
    time.sleep(0.2)
    a.kill_hard(); time.sleep(0.4)
    if srv.death():
        return
    b = C(srv.port); out = b.register("bob", srv.password)
    check("001" in out, "server fine after a client dies mid-command")
    b.close()


def t_frozen_client_flood(srv):
    """SIGSTOP equivalent: a client that never reads, flooded by another."""
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.send("JOIN #flood"); b.send("JOIN #flood"); a.read(); b.read()
    # bob never calls recv -> his socket buffer fills up
    payload = "x" * 400
    t0 = time.time()
    for i in range(3000):
        a.send(f"PRIVMSG #flood :{i} {payload}")
    elapsed = time.time() - t0
    check(elapsed < 20, f"server does not hang while flooding a non-reading client ({elapsed:.1f}s)")
    if srv.death():
        return
    c = C(srv.port); out = c.register("carol", srv.password)
    check("001" in out, "server still accepts new clients during the flood")
    got = b.read(1.5)
    check(len(got) > 0, "frozen client receives its backlog once it reads again "
                        f"({len(got)} bytes)")
    a.close(); b.close(); c.close()


def t_long_line(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.send("PRIVMSG #x :" + "A" * 4000)
    time.sleep(0.3)
    a.send("PING :after-long")
    got = a.read(0.8)
    check("PONG" in got, "a very long line does not break the parser", repr(got[-120:]))
    a.close()


def t_empty_and_junk(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    for junk in ["", " ", ":", "::::", ":prefix", "  PING  :spaced", "\t", "PRIVMSG :"]:
        a.send(junk)
        time.sleep(0.05)
    time.sleep(0.3)
    a.send("PING :survived"); got = a.read(0.8)
    check("PONG" in got, "malformed / empty lines do not kill the server", repr(got[:160]))
    a.close()


# ───────────────────────── 4. registration ─────────────────────────

def t_wrong_password(srv):
    a = C(srv.port)
    a.send("PASS wrongpassword")
    got = a.read(0.8)
    check("464" in got, "wrong PASS returns 464", repr(got[:160]))
    time.sleep(0.3)
    if srv.death():
        return
    b = C(srv.port); out = b.register("bob", srv.password)
    check("001" in out, "server survives a rejected password")
    a.close(); b.close()


def t_no_pass(srv):
    a = C(srv.port)
    a.send("NICK alice"); a.send("USER alice 0 * :A")
    got = a.read(0.8)
    check("001" not in got, "cannot register without PASS", repr(got[:160]))
    check("451" in got or "464" in got, "returns 451/464 when PASS is missing", repr(got[:160]))
    a.close()


def t_nick_collision(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    b = C(srv.port)
    b.send(f"PASS {srv.password}"); b.send("NICK alice")
    got = b.read(0.8)
    check("433" in got, "duplicate nickname returns 433", repr(got[:160]))
    a.close(); b.close()


def t_bad_nick(srv):
    a = C(srv.port)
    a.send(f"PASS {srv.password}")
    bad = ["ni!ck", "ni ck".replace(" ", "@"), "#chan", "1abc", "a" * 40]
    ok = True
    for nick in bad:
        a.send("NICK " + nick)
        got = a.read(0.5)
        if "432" not in got and "431" not in got:
            ok = False
            detail = f"{nick!r} accepted -> {got[:100]!r}"
            break
    check(ok, "invalid nicknames return 432", "" if ok else detail)
    a.send("NICK")
    got = a.read(0.6)
    check("431" in got or "461" in got, "NICK with no parameter returns 431", repr(got[:160]))
    a.close()


def t_nick_change(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.send("JOIN #nc"); b.send("JOIN #nc"); a.read(); b.read()
    a.send("NICK alice2")
    got_a = a.read(0.8)
    check("NICK" in got_a, "NICK change is confirmed to the user", repr(got_a[:160]))
    got_b = b.read(0.5)
    check("alice2" in got_b, "NICK change is relayed to channel members", repr(got_b[:160]))
    a.close(); b.close()


def t_reregister(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.send(f"PASS {srv.password}")
    got = a.read(0.6)
    check("462" in got, "PASS after registration returns 462", repr(got[:160]))
    a.send("USER x 0 * :x")
    got = a.read(0.6)
    check("462" in got, "USER after registration returns 462", repr(got[:160]))
    a.close()


def t_unknown_command(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.send("NOTACOMMAND foo")
    got = a.read(0.8)
    check("421" in got, "unknown command returns 421", repr(got[:160]))
    a.close()


def t_quit(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.send("JOIN #q"); b.send("JOIN #q"); a.read(); b.read()
    a.send("QUIT :goodbye")
    time.sleep(0.5)
    if srv.death():
        return
    got = b.read(0.8)
    check("QUIT" in got, "QUIT is relayed to channel members", repr(got[:160]))
    b.send("PING :alive"); got = b.read(0.6)
    check("PONG" in got, "server healthy after a QUIT")
    b.close()


# ───────────────────────── 5. channel operator ─────────────────────────

def t_join_modes(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.send("JOIN #ops"); a.read()

    a.send("MODE #ops +k secret"); a.read(0.5)
    b.send("JOIN #ops"); got = b.read(0.8)
    check("475" in got, "+k rejects a JOIN without the key (475)", repr(got[:160]))
    b.send("JOIN #ops secret"); got = b.read(0.8)
    check("JOIN" in got and "475" not in got, "+k accepts a JOIN with the right key", repr(got[:160]))

    a.send("MODE #ops -k"); a.read(0.4)
    c = C(srv.port); c.register("carol", srv.password); c.read()
    c.send("JOIN #ops"); got = c.read(0.8)
    check("JOIN" in got, "-k removes the key", repr(got[:160]))
    a.close(); b.close(); c.close()


def t_invite_only(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    a.send("JOIN #inv"); a.read()
    a.send("MODE #inv +i"); a.read(0.4)
    b.send("JOIN #inv"); got = b.read(0.8)
    check("473" in got, "+i blocks an uninvited JOIN (473)", repr(got[:160]))
    a.send("INVITE bob #inv"); got_a = a.read(0.5); got_b = b.read(0.5)
    check("341" in got_a, "INVITE returns 341 to the inviter", repr(got_a[:160]))
    check("INVITE" in got_b, "invited user receives the INVITE", repr(got_b[:160]))
    b.send("JOIN #inv"); got = b.read(0.8)
    check("JOIN" in got and "473" not in got, "invited user can now JOIN", repr(got[:160]))
    a.close(); b.close()


def t_user_limit(srv):
    a = C(srv.port); a.register("alice", srv.password)
    a.send("JOIN #lim"); a.read()
    a.send("MODE #lim +l 1"); a.read(0.4)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    b.send("JOIN #lim"); got = b.read(0.8)
    check("471" in got, "+l blocks a JOIN over the limit (471)", repr(got[:160]))
    a.send("MODE #lim -l"); a.read(0.4)
    b.send("JOIN #lim"); got = b.read(0.8)
    check("JOIN" in got, "-l removes the limit", repr(got[:160]))
    a.close(); b.close()


def t_topic(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    a.send("JOIN #top"); a.read()
    b.send("JOIN #top"); b.read()

    a.send("TOPIC #top"); got = a.read(0.8)
    check("331" in got, "TOPIC with no topic set returns 331", repr(got[:160]))
    if srv.death():
        return

    a.send("TOPIC #top :the new topic"); a.read(0.4)
    got = b.read(0.5)
    check("the new topic" in got, "TOPIC change is broadcast to the channel", repr(got[:160]))
    b.send("TOPIC #top"); got = b.read(0.8)
    check("332" in got and "the new topic" in got, "TOPIC query returns 332 + the topic",
          repr(got[:160]))

    a.send("MODE #top +t"); a.read(0.4)
    b.send("TOPIC #top :bob hijacks"); got = b.read(0.8)
    check("482" in got, "+t stops a non-operator changing the topic (482)", repr(got[:160]))
    a.send("MODE #top -t"); a.read(0.4)
    b.send("TOPIC #top :now allowed"); got = b.read(0.8)
    check("482" not in got, "-t lets a regular user change the topic", repr(got[:160]))
    a.close(); b.close()


def t_kick(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    a.send("JOIN #k"); a.read(); b.send("JOIN #k"); b.read(); a.read()

    b.send("KICK #k alice :try me"); got = b.read(0.8)
    check("482" in got, "a regular user cannot KICK (482)", repr(got[:160]))
    if srv.death():
        return
    a.send("KICK #k bob :out you go")
    got_b = b.read(0.8); got_a = a.read(0.4)
    check("KICK" in got_b, "kicked user is notified", repr(got_b[:160]))
    check("KICK" in got_a, "operator sees the KICK too", repr(got_a[:160]))
    b.send("PRIVMSG #k :am I still here?"); time.sleep(0.3)
    got = a.read(0.5)
    check("still here" not in got, "kicked user really left the channel")
    a.send("KICK #k ghost"); got = a.read(0.6)
    check("401" in got or "441" in got, "KICK on a non-member returns 401/441", repr(got[:160]))
    a.close(); b.close()


def t_mode_o(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    a.send("JOIN #o"); a.read(); b.send("JOIN #o"); b.read(); a.read()
    b.send("MODE #o +t"); got = b.read(0.8)
    check("482" in got, "regular user cannot change modes (482)", repr(got[:160]))
    if srv.death():
        return
    a.send("MODE #o +o bob"); a.read(0.5); b.read(0.5)
    b.send("MODE #o +t"); got = b.read(0.8)
    check("482" not in got, "+o grants operator rights", repr(got[:160]))
    a.send("MODE #o -o bob"); a.read(0.5); b.read(0.5)
    b.send("MODE #o +i"); got = b.read(0.8)
    check("482" in got, "-o removes operator rights", repr(got[:160]))
    a.close(); b.close()


def t_first_joiner_is_op(srv):
    a = C(srv.port); a.register("alice", srv.password)
    a.send("JOIN #first"); got = a.read(0.8)
    check("@alice" in got, "first joiner is listed as operator in NAMES", repr(got[:200]))
    a.send("MODE #first +i"); got = a.read(0.6)
    check("482" not in got, "first joiner actually has operator rights", repr(got[:160]))
    a.close()


def t_part(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    a.send("JOIN #p"); a.read(); b.send("JOIN #p"); b.read(); a.read()
    b.send("PART #p :bye"); got_a = a.read(0.8)
    check("PART" in got_a, "PART is broadcast to remaining members", repr(got_a[:160]))
    a.send("PRIVMSG #p :still there?"); time.sleep(0.3)
    got = b.read(0.5)
    check("still there" not in got, "user who PARTed no longer receives channel messages")
    b.send("PART #nosuch"); got = b.read(0.6)
    check("403" in got or "442" in got, "PART on unknown channel returns 403", repr(got[:160]))
    a.close(); b.close()


def t_op_errors(srv):
    """The classic crashers: operator commands with missing/bad arguments."""
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.send("JOIN #e"); a.read()
    probes = [
        ("JOIN", "461"), ("JOIN #", None), ("JOIN nohash", "403"),
        ("KICK", "461"), ("KICK #e", "461"), ("KICK #nosuch bob", "403"),
        ("INVITE", "461"), ("INVITE bob", "461"), ("INVITE bob #nosuch", "403"),
        ("TOPIC", "461"), ("TOPIC #nosuch", "403"), ("TOPIC #e", "331"),
        ("MODE", "461"), ("MODE #e", None), ("MODE #nosuch +i", "403"),
        ("MODE #e +k", "461"), ("MODE #e +l", "461"), ("MODE #e +o", "461"),
        ("MODE #e +l abc", None), ("MODE #e +z", None), ("MODE #e +o ghost", "401|441"),
        ("PART", "461"), ("PRIVMSG #nosuch :hi", "403"),
    ]
    for cmd, expect_code in probes:
        a.send(cmd)
        got = a.read(0.35)
        if srv.death():
            record(f'"{cmd}" does not kill the server', "CRASH", srv.death())
            return
        if expect_code:
            alts = expect_code.split("|")
            check(any(c in got for c in alts),
                  f'"{cmd}" -> {" or ".join(alts)}', repr(got[:140]) or "no reply")
        else:
            record(f'"{cmd}" handled without crashing', "PASS")
    a.send("PING :end"); got = a.read(0.6)
    check("PONG" in got, "server responsive after all the bad-argument probes")
    a.close()


# ───────────────────────── main ─────────────────────────

SECTIONS = {
    "networking": [
        ("listen + answer", t_listen_and_answer),
        ("multiple clients", t_multi_clients),
        ("PING/PONG", t_ping),
        ("channel broadcast", t_channel_broadcast),
        ("PRIVMSG", t_privmsg_direct),
    ],
    "specials": [
        ("partial command", t_partial_command),
        ("many commands, one packet", t_multiple_commands_one_packet),
        ("bare LF endings", t_bare_lf),
        ("abruptly killed client", t_abrupt_kill),
        ("client killed mid-command", t_half_command_kill),
        ("flood a frozen client", t_frozen_client_flood),
        ("very long line", t_long_line),
        ("empty / junk lines", t_empty_and_junk),
    ],
    "registration": [
        ("wrong password", t_wrong_password),
        ("no PASS", t_no_pass),
        ("nickname collision", t_nick_collision),
        ("invalid nickname", t_bad_nick),
        ("nickname change", t_nick_change),
        ("re-registration", t_reregister),
        ("unknown command", t_unknown_command),
        ("QUIT", t_quit),
    ],
    "channel": [
        ("first joiner is operator", t_first_joiner_is_op),
        ("channel key +k", t_join_modes),
        ("invite only +i", t_invite_only),
        ("user limit +l", t_user_limit),
        ("TOPIC and +t", t_topic),
        ("KICK", t_kick),
        ("operator flag +o", t_mode_o),
        ("PART", t_part),
        ("bad arguments on every command", t_op_errors),
    ],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./ircserv")
    ap.add_argument("--port", type=int, default=6767)
    ap.add_argument("--password", default="testpass")
    ap.add_argument("--src", default="src")
    ap.add_argument("--only", default=None, help="networking|specials|registration|channel")
    ap.add_argument("--no-static", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        print(f"{R}binary {args.binary} not found - run make first{N}")
        sys.exit(1)

    print(f"{B}ft_irc tester{N}  binary={args.binary} port={args.port} password={args.password}")

    if not args.no_static:
        static_checks(args.src)

    port = args.port
    for sec, tests in SECTIONS.items():
        if args.only and args.only != sec:
            continue
        section(sec)
        for name, fn in tests:
            srv = Server(args.binary, port, args.password)
            port += 1
            group(srv, name, fn)

    print(f"\n{B}=== summary ==={N}")
    counts = {}
    for _, _, st, _ in results:
        counts[st] = counts.get(st, 0) + 1
    for st in ("PASS", "FAIL", "CRASH", "WARN", "SKIP"):
        if counts.get(st):
            col = {"PASS": G, "FAIL": R, "CRASH": R, "WARN": Y, "SKIP": D}[st]
            print(f"  {col}{st:5}{N} {counts[st]}")

    crashes = [(s, n, d) for s, n, st, d in results if st == "CRASH"]
    if crashes:
        print(f"\n{R}CRASHES - each of these is a 0 at defense:{N}")
        for s, n, d in crashes:
            print(f"  - [{s}] {n}: {d}")

    fails = [(s, n, d) for s, n, st, d in results if st == "FAIL"]
    if fails:
        print(f"\n{R}FAILURES:{N}")
        for s, n, d in fails:
            print(f"  - [{s}] {n}")

    sys.exit(1 if (crashes or fails) else 0)


if __name__ == "__main__":
    main()