#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ft_irc tester v2 - mirrors the 42 evaluation sheet.

usage:
    python3 irc_tester.py                       # build + run everything
    python3 irc_tester.py --only channel        # one section
    python3 irc_tester.py --list                # list section names
    python3 irc_tester.py --no-build            # skip the make checks
    python3 irc_tester.py --binary ./ircserv --password pass

Exit code is 0 only when there is no FAIL and no CRASH. WARN never blocks:
those are RFC niceties or advisory checks, not sheet requirements.

WHAT THIS CANNOT TEST FOR YOU
  - Connecting with your reference IRC client (irssi / HexChat / WeeChat).
    The sheet requires it to connect "without encountering any error".
    Do that by hand before you push.
  - An evaluator reading your poll() loop and asking you to explain it.
"""

import argparse
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time

G = "\033[32m"; R = "\033[31m"; Y = "\033[33m"; B = "\033[36m"; D = "\033[2m"; N = "\033[0m"
COLOR = {"PASS": G, "FAIL": R, "CRASH": R, "WARN": Y, "SKIP": D}

results = []
CUR_SECTION = "misc"


def record(name, status, detail=""):
    results.append((CUR_SECTION, name, status, detail))
    line = f"  {COLOR[status]}[{status:5}]{N} {name}"
    if detail:
        line += f"\n          {D}{detail}{N}"
    print(line)


def check(cond, name, detail="", level="FAIL"):
    record(name, "PASS" if cond else level, "" if cond else detail)
    return cond


def advisory(cond, name, detail=""):
    """Fails as WARN - RFC nicety or environment-dependent, will not block a push."""
    return check(cond, name, detail, level="WARN")


def section(title):
    global CUR_SECTION
    CUR_SECTION = title
    print(f"\n{B}=== {title} ==={N}")


# ───────────────────────── server process ─────────────────────────

class Server:
    def __init__(self, binary, port, password, wrapper=None):
        self.binary, self.port, self.password = binary, port, password
        self.wrapper = wrapper or []
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            self.wrapper + [self.binary, str(self.port), self.password],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for _ in range(80):
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
                self.proc.communicate(timeout=3)
            except Exception:
                pass


class C:
    """One IRC connection."""

    def __init__(self, port):
        self.buf = ""
        self.s = socket.create_connection(("127.0.0.1", port), 3)
        self.s.settimeout(0.2)

    def send(self, line):
        self.s.sendall((line + "\r\n").encode())
        return self

    def raw(self, data):
        self.s.sendall(data if isinstance(data, bytes) else data.encode())
        return self

    def read(self, seconds=0.6):
        end = time.time() + seconds
        while time.time() < end:
            try:
                chunk = self.s.recv(8192)
                if not chunk:
                    break
                self.buf += chunk.decode(errors="replace")
                end = time.time() + 0.15
            except socket.timeout:
                continue
            except OSError:
                break
        out, self.buf = self.buf, ""
        return out

    def register(self, nick, password, user=None):
        user = user or nick
        self.send(f"PASS {password}")
        self.send(f"NICK {nick}")
        self.send(f"USER {user} 0 * :Real {nick}")
        return self.read(0.9)

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def kill_hard(self):
        """RST instead of FIN - an abruptly killed client."""
        try:
            self.s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            self.s.close()
        except Exception:
            self.close()


def group(srv, name, fn):
    if not srv.start():
        record(name, "CRASH", "server would not start / bind: " + (srv.death() or "timeout"))
        return
    try:
        fn(srv)
    except (ConnectionResetError, BrokenPipeError, ConnectionRefusedError, socket.timeout) as e:
        d = srv.death()
        record(name + " (connection lost)", "CRASH" if d else "FAIL", d or repr(e))
    except Exception as e:
        record(name, "FAIL", f"tester error: {type(e).__name__}: {e}")
    finally:
        d = srv.death()
        if d:
            record(f"server survived: {name}", "CRASH", d)
        srv.stop()


# ───────────────────────── 0. build / Makefile ─────────────────────────

def build_checks(project, binary):
    section("build")
    mk = os.path.join(project, "Makefile")
    if not os.path.exists(mk):
        record("Makefile exists", "FAIL", f"no Makefile in {project}")
        return
    record("Makefile exists", "PASS")
    body = open(mk, errors="replace").read()

    for rule in ["all", "clean", "fclean", "re"]:
        check(re.search(rf"^{rule}\s*:", body, re.M) is not None, f"rule `{rule}` present")
    check("NAME" in body, "NAME variable present")

    flags = re.search(r"(CXXFLAGS|CFLAGS)\s*[:+]?=\s*(.*)", body)
    fl = flags.group(2) if flags else ""
    for f in ["-Wall", "-Wextra", "-Werror"]:
        check(f in fl, f"compiles with {f}", repr(fl))
    advisory("-std=c++98" in fl, "compiles with -std=c++98", repr(fl))

    def run(*a):
        return subprocess.run(["make", "-C", project] + list(a),
                              capture_output=True, text=True, timeout=300)

    run("fclean")
    r = run()
    check(r.returncode == 0, "`make` from clean succeeds", (r.stderr or r.stdout)[-400:])
    check(os.path.exists(binary), f"produces the executable ({os.path.basename(binary)})")

    r = run()
    check("Nothing to be done" in r.stdout or not r.stdout.strip(),
          "`make` twice does not relink", repr(r.stdout[-200:]))

    hdrs = [os.path.join(dp, f) for dp, _, fs in os.walk(project)
            for f in fs if f.endswith(".hpp")]
    if hdrs:
        os.utime(hdrs[0], None)
        r = run()
        advisory("Nothing to be done" not in r.stdout,
                 "editing a header triggers a rebuild",
                 f"touching {os.path.basename(hdrs[0])} rebuilt nothing - add header deps "
                 "(-MMD -MP, or $(OBJS): $(HEADERS)). You will hit this during the "
                 "'small modification' step at defense.")

    run("fclean")
    check(not os.path.exists(binary), "`fclean` removes the executable")
    run()


# ───────────────────────── 1. static source checks ─────────────────────────

def static_checks(srcdir):
    section("static")
    files = [os.path.join(dp, f) for dp, _, fs in os.walk(srcdir) for f in fs
             if f.endswith((".cpp", ".hpp", ".h", ".tpp", ".ipp")) and ".git" not in dp]
    if not files:
        record("source files found", "SKIP", f"nothing under {srcdir}")
        return
    text = "\n".join(open(f, errors="replace").read() for f in files)
    code = re.sub(r"//[^\n]*", "", text)
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    code = re.sub(r'"(\\.|[^"\\])*"', '""', code)

    polls = len(re.findall(r"\b(poll|select|kqueue|kevent|epoll_wait)\s*\(", code))
    check(polls == 1, f"exactly one poll()/equivalent call site ({polls} found)",
          "the sheet is explicit: more than one ends the evaluation at 0")

    bad = [m for m in re.findall(r"fcntl\s*\([^;]*\)", code)
           if not re.search(r"F_SETFL\s*,\s*O_NONBLOCK", m)]
    check(not bad, "fcntl only as fcntl(fd, F_SETFL, O_NONBLOCK)", "; ".join(bad[:3]))

    check(not re.search(r"\bfork\s*\(", code), "no fork()")
    check(not re.search(r"\b(pthread_create|std::thread)\b", code), "no threads")

    errs = re.findall(r"errno\s*[=!]=\s*(EAGAIN|EWOULDBLOCK|EINTR)", code)
    check(not errs, "no errno-driven action after recv/send",
          f"{len(errs)} comparison(s); the sheet forbids using errno to trigger an "
          "action after read/recv/write/send")

    c11 = re.findall(r"\bstd::(unordered_map|unordered_set|shared_ptr|unique_ptr|to_string)\b"
                     r"|\bnullptr\b", code)
    check(not c11, "no obvious C++11 constructs", f"found: {sorted(set(c11)) if c11 else ''}")


# ───────────────────────── 2. launch arguments ─────────────────────────

def arg_checks(binary, password):
    section("arguments")

    def run(args, timeout=3):
        try:
            p = subprocess.run([binary] + args, capture_output=True, text=True, timeout=timeout)
            return p.returncode, (p.stdout + p.stderr)
        except subprocess.TimeoutExpired:
            return None, "<did not exit>"

    for label, args in [("no arguments", []), ("only a port", ["6667"]),
                        ("too many arguments", ["6667", "pw", "extra"]),
                        ("non-numeric port", ["abc", "pw"]),
                        ("port out of range", ["99999999", "pw"]),
                        ("port 0", ["0", "pw"]),
                        ("negative port", ["-5", "pw"])]:
        rc, out = run(args)
        check(rc is not None and rc != 0 and out.strip() != "",
              f"rejects {label} with a message and non-zero exit",
              f"rc={rc} out={out.strip()[:120]!r}")

    advisory(run(["6667", ""])[0] not in (0, None), "rejects an empty password",
             "some teams allow it; decide and be consistent")

    port = 7801
    a = Server(binary, port, password)
    if a.start():
        rc, out = run([str(port), password])
        check(rc is not None and rc != 0,
              "refuses to start when the port is already in use",
              f"rc={rc} out={out.strip()[:120]!r}")
        a.stop()
    else:
        record("port already in use", "SKIP", "could not bind the first server")


# ───────────────────────── 3. networking ─────────────────────────

def t_welcome(srv):
    a = C(srv.port)
    out = a.register("alice", srv.password)
    check("001" in out, "registration returns RPL_WELCOME (001)", repr(out[:180]))
    check("004" in out, "registration returns the 001-004 block", repr(out[:220]))
    a.close()


def t_multi_clients(srv):
    cs = [C(srv.port) for _ in range(5)]
    for i, c in enumerate(cs):
        c.register(f"user{i}", srv.password)
    ok = True
    for c in cs:
        c.send("PING :probe")
        if "PONG" not in c.read(0.6):
            ok = False
    check(ok, "5 simultaneous clients all get answered")
    for c in cs:
        c.close()


def t_many_clients(srv):
    cs = []
    try:
        for i in range(40):
            c = C(srv.port)
            c.register(f"bulk{i}", srv.password)
            cs.append(c)
        for c in cs:
            c.send("JOIN #big")
        time.sleep(1.2)
        for c in cs:
            c.read(0.2)
        cs[0].send("PRIVMSG #big :broadcast to forty")
        got = cs[-1].read(1.5)
        check("broadcast to forty" in got,
              "40 clients in one channel all receive a broadcast", repr(got[-160:]))
    finally:
        for c in cs:
            c.close()


def t_ping(srv):
    a = C(srv.port); a.register("alice", srv.password)
    a.send("PING :hello"); out = a.read()
    check("PONG" in out and "hello" in out, "PING returns PONG with the token", repr(out[:140]))
    a.close()


def t_channel_broadcast(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.send("JOIN #test"); out = a.read(0.9)
    check("JOIN" in out, "JOIN is echoed to the joiner", repr(out[:200]))
    check("353" in out and "366" in out, "JOIN sends NAMES (353) and end of NAMES (366)",
          repr(out[:220]))
    b.send("JOIN #test"); b.read(0.9)
    out = a.read(0.7)
    check("bob" in out and "JOIN" in out, "existing members are told about a new joiner",
          repr(out[:200]))
    a.send("PRIVMSG #test :hello channel")
    got = b.read(0.9)
    check("hello channel" in got, "channel PRIVMSG reaches other members", repr(got[:200]))
    check("hello channel" not in a.read(0.3),
          "the sender does not get its own channel message back")
    a.close(); b.close()


def t_privmsg(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.read(); b.read()
    a.send("PRIVMSG bob :private hi")
    check("private hi" in b.read(0.9), "PRIVMSG to a nick reaches the target")
    a.send("PRIVMSG ghost :nobody")
    check("401" in a.read(0.7), "PRIVMSG to an unknown nick returns 401")
    a.send("PRIVMSG")
    got = a.read(0.7)
    check("411" in got or "461" in got, "PRIVMSG with no target returns 411/461", repr(got[:140]))
    a.send("PRIVMSG bob")
    got = a.read(0.7)
    check("412" in got or "461" in got, "PRIVMSG with no text returns 412/461", repr(got[:140]))
    a.send("PRIVMSG bob :trailing :colons: inside")
    check("trailing :colons: inside" in b.read(0.9), "colons inside the trailing text survive")
    a.send("PRIVMSG bob :   leading spaces kept")
    check("   leading spaces kept" in b.read(0.9),
          "leading spaces in the trailing parameter are preserved")
    a.close(); b.close()


def t_multi_target(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    c = C(srv.port); c.register("carol", srv.password)
    a.read(); b.read(); c.read()
    a.send("PRIVMSG bob,carol :to both of you")
    gb, gc = b.read(0.9), c.read(0.9)
    check("to both of you" in gb and "to both of you" in gc,
          "PRIVMSG with a comma-separated target list reaches every target",
          f"bob={gb[:80]!r} carol={gc[:80]!r}")
    a.close(); b.close(); c.close()


def t_all_interfaces(srv):
    """The sheet: 'listens on all network interfaces'."""
    ip = None
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("8.8.8.8", 80))
        ip = probe.getsockname()[0]
        probe.close()
    except OSError:
        pass
    if not ip or ip.startswith("127."):
        record("listens on all interfaces (0.0.0.0)", "SKIP",
               "no non-loopback address here; check by hand with your LAN IP")
        return
    try:
        socket.create_connection((ip, srv.port), 2).close()
        record(f"listens on all interfaces (reached on {ip})", "PASS")
    except OSError as e:
        record("listens on all interfaces (0.0.0.0)", "FAIL",
               f"could not connect on {ip}: {e} - bind INADDR_ANY, not 127.0.0.1")


# ───────────────────────── 4. networking specials ─────────────────────────

def t_partial(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    b = C(srv.port); b.register("bob", srv.password); b.read()
    for piece in ["PI", "NG :sp", "lit"]:
        a.raw(piece); time.sleep(0.15)
    check("PONG" not in a.read(0.3), "a partial command is buffered, not executed early")
    a.raw("\r\n")
    got = a.read(0.9)
    check("PONG" in got and "split" in got, "the partial command completes once CRLF arrives",
          repr(got[:160]))
    b.send("PING :other")
    check("PONG" in b.read(0.7), "other clients keep working while one command is pending")
    a.close(); b.close()


def t_byte_at_a_time(srv):
    a = C(srv.port)
    for ch in f"PASS {srv.password}\r\nNICK slow\r\nUSER slow 0 * :S\r\n":
        a.raw(ch); time.sleep(0.005)
    check("001" in a.read(1.2), "registration works when sent one byte at a time")
    a.close()


def t_batched(srv):
    a = C(srv.port)
    a.raw(f"PASS {srv.password}\r\nNICK alice\r\nUSER alice 0 * :A\r\nPING :x\r\n")
    got = a.read(1.2)
    check("001" in got and "PONG" in got, "several commands in one packet are all processed",
          repr(got[:200]))
    a.close()


def t_bare_lf(srv):
    a = C(srv.port)
    a.raw(f"PASS {srv.password}\nNICK lf\nUSER lf 0 * :L\n")
    check("001" in a.read(1.2), "accepts bare LF endings (plain nc, without -C)",
          "the server only splits on CRLF; a grader using plain `nc` sees a dead server")
    a.close()


def t_abrupt_kill(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.send("JOIN #room"); b.send("JOIN #room"); a.read(); b.read()
    a.kill_hard(); time.sleep(0.5)
    if srv.death():
        return
    b.send("PING :alive")
    check("PONG" in b.read(0.9), "server survives a client killed abruptly (RST)")
    c = C(srv.port)
    check("001" in c.register("carol", srv.password), "new clients connect after an abrupt kill")
    c.send("JOIN #room")
    check("353" in c.read(0.9), "joining a channel a dead client was in still works")
    b.send("PRIVMSG #room :after death")
    check("after death" in c.read(0.9),
          "broadcasting to a channel a dead client was in works (dangling Client* check)")
    b.close(); c.close()


def t_half_command_kill(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.raw("PRIVMSG #room :half of a comm")
    time.sleep(0.25)
    a.kill_hard(); time.sleep(0.5)
    if srv.death():
        return
    b = C(srv.port)
    check("001" in b.register("bob", srv.password), "server fine after a client dies mid-command")
    b.close()


def t_churn(srv):
    for i in range(60):
        try:
            c = C(srv.port)
            if i % 3 == 0:
                c.raw(f"PASS {srv.password}\r\nNICK churn{i}\r\n")
            elif i % 3 == 1:
                c.raw("PART")
            c.kill_hard()
        except OSError:
            break
    time.sleep(0.5)
    if srv.death():
        return
    a = C(srv.port)
    check("001" in a.register("survivor", srv.password),
          "server healthy after 60 rapid connect/disconnect cycles")
    a.close()


def t_frozen_flood(srv):
    """The Ctrl-Z scenario: a client that never reads, flooded by another."""
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    try:
        b.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
    except OSError:
        pass
    a.send("JOIN #flood"); b.send("JOIN #flood"); a.read(); b.read()
    payload = "x" * 400
    t0 = time.time()
    for i in range(3000):
        a.send(f"PRIVMSG #flood :{i} {payload}")
    elapsed = time.time() - t0
    check(elapsed < 20, f"server does not hang while flooding a frozen client ({elapsed:.1f}s)")
    if srv.death():
        return
    c = C(srv.port)
    check("001" in c.register("carol", srv.password), "new clients still accepted during a flood")
    got = b.read(2.0)
    check(len(got) > 0,
          f"the frozen client gets its backlog once it reads again ({len(got)} bytes)")
    a.send("PING :end")
    check("PONG" in a.read(1.0), "the flooding client is still served")
    a.close(); b.close(); c.close()


def t_long_line(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.send("PRIVMSG #x :" + "A" * 4000)
    time.sleep(0.4)
    a.send("PING :after-long")
    check("PONG" in a.read(1.0), "a 4 KB line does not break the parser")
    a.raw("Z" * 20000)
    time.sleep(0.3)
    a.send("")
    a.send("PING :after-garbage")
    got = a.read(1.0)
    advisory("PONG" in got, "20 KB with no terminator does not wedge the connection",
             "cap the read buffer; IRC lines are 512 bytes max")
    a.close()


def t_junk(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    for junk in ["", " ", ":", "::::", ":prefix", "  PING  :spaced", "\t",
                 "PRIVMSG :", "MODE " + "#" * 300, ":a b c d e f g"]:
        a.send(junk); time.sleep(0.05)
    time.sleep(0.4)
    a.send("PING :survived")
    check("PONG" in a.read(1.0), "malformed and empty lines do not kill the server")
    a.close()


# ───────────────────────── 5. registration ─────────────────────────

def t_wrong_pass(srv):
    a = C(srv.port)
    a.send("PASS wrongpassword")
    check("464" in a.read(1.0), "wrong PASS returns 464 before the socket closes",
          "if this is empty, the reply is queued and the fd is closed before POLLOUT flushes")
    time.sleep(0.4)
    if srv.death():
        return
    b = C(srv.port)
    check("001" in b.register("bob", srv.password), "server survives a rejected password")
    a.close(); b.close()


def t_no_pass(srv):
    a = C(srv.port)
    a.send("NICK alice"); a.send("USER alice 0 * :A")
    got = a.read(1.0)
    check("001" not in got, "cannot register without PASS", repr(got[:160]))
    check("451" in got or "464" in got, "returns 451/464 when PASS is missing", repr(got[:160]))
    a.close()


def t_reg_order(srv):
    a = C(srv.port)
    a.send(f"PASS {srv.password}")
    a.send("USER alice 0 * :A")
    a.send("NICK alice")
    check("001" in a.read(1.2), "registration works with USER before NICK")
    a.close()


def t_nick_collision(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    b = C(srv.port)
    b.send(f"PASS {srv.password}"); b.send("NICK alice")
    check("433" in b.read(1.0), "duplicate nickname returns 433")
    b.send("NICK ALICE")
    advisory("433" in b.read(1.0), "nickname collision is case-insensitive",
             "RFC 2812: nicks compare case-insensitively, so ALICE collides with alice")
    a.close(); b.close()


def t_bad_nick(srv):
    a = C(srv.port)
    a.send(f"PASS {srv.password}")
    ok, detail = True, ""
    for nick in ["ni!ck", "ni@ck", "#chan", "1abc", "a" * 40, "ni,ck"]:
        a.send("NICK " + nick)
        got = a.read(0.5)
        if "432" not in got and "431" not in got:
            ok, detail = False, f"{nick!r} accepted -> {got[:100]!r}"
            break
    check(ok, "invalid nicknames return 432", detail)
    a.send("NICK")
    got = a.read(0.7)
    check("431" in got or "461" in got, "NICK with no parameter returns 431", repr(got[:160]))
    a.close()


def t_nick_change(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.send("JOIN #nc"); b.send("JOIN #nc"); a.read(); b.read()
    a.send("NICK alice2")
    check("NICK" in a.read(1.0), "NICK change is confirmed to the user")
    check("alice2" in b.read(0.7), "NICK change is relayed to channel members")
    a.send("PRIVMSG #nc :from my new nick")
    check("alice2" in b.read(0.9), "messages after a NICK change carry the new prefix")
    a.close(); b.close()


def t_reregister(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.send(f"PASS {srv.password}")
    check("462" in a.read(0.7), "PASS after registration returns 462")
    a.send("USER x 0 * :x")
    check("462" in a.read(0.7), "USER after registration returns 462")
    a.close()


def t_unknown_cmd(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.send("NOTACOMMAND foo")
    check("421" in a.read(1.0), "unknown command returns 421")
    a.close()


def t_quit(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.send("JOIN #q"); b.send("JOIN #q"); a.read(); b.read()
    a.send("QUIT :goodbye")
    time.sleep(0.6)
    if srv.death():
        return
    check("QUIT" in b.read(1.0), "QUIT is relayed to channel members")
    b.send("PING :alive")
    check("PONG" in b.read(0.7), "server healthy after a QUIT")
    b.close()


# ───────────────────────── 6. channel operator ─────────────────────────

def t_first_op(srv):
    a = C(srv.port); a.register("alice", srv.password)
    a.send("JOIN #first")
    check("@alice" in a.read(1.0), "the first joiner is listed as operator in NAMES")
    a.send("MODE #first +i")
    check("482" not in a.read(0.7), "the first joiner actually holds operator rights")
    a.close()


def t_key(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.send("JOIN #ops"); a.read()
    a.send("MODE #ops +k secret"); a.read(0.6)
    b.send("JOIN #ops")
    check("475" in b.read(1.0), "+k rejects a JOIN with no key (475)")
    b.send("JOIN #ops wrongkey")
    check("475" in b.read(1.0), "+k rejects a JOIN with the wrong key")
    b.send("JOIN #ops secret")
    got = b.read(1.0)
    check("JOIN" in got and "475" not in got, "+k accepts a JOIN with the right key")
    a.send("MODE #ops -k"); a.read(0.5)
    c = C(srv.port); c.register("carol", srv.password); c.read()
    c.send("JOIN #ops")
    check("JOIN" in c.read(1.0), "-k removes the key")
    a.close(); b.close(); c.close()


def t_invite(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    a.send("JOIN #inv"); a.read()
    a.send("MODE #inv +i"); a.read(0.5)
    b.send("JOIN #inv")
    check("473" in b.read(1.0), "+i blocks an uninvited JOIN (473)")
    a.send("INVITE bob #inv")
    check("341" in a.read(0.7), "INVITE returns 341 to the inviter")
    check("INVITE" in b.read(0.7), "the invited user receives the INVITE")
    b.send("JOIN #inv")
    got = b.read(1.0)
    check("JOIN" in got and "473" not in got, "the invited user can then JOIN")
    a.close(); b.close()


def t_limit(srv):
    a = C(srv.port); a.register("alice", srv.password)
    a.send("JOIN #lim"); a.read()
    a.send("MODE #lim +l 1"); a.read(0.5)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    b.send("JOIN #lim")
    check("471" in b.read(1.0), "+l blocks a JOIN over the limit (471)")
    a.send("MODE #lim -l"); a.read(0.5)
    b.send("JOIN #lim")
    check("JOIN" in b.read(1.0), "-l removes the limit")
    a.close(); b.close()


def t_topic(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    a.send("JOIN #top"); a.read()
    b.send("JOIN #top"); b.read()
    a.send("TOPIC #top")
    check("331" in a.read(1.0), "TOPIC with none set returns 331")
    if srv.death():
        return
    a.send("TOPIC #top :the new topic"); a.read(0.5)
    check("the new topic" in b.read(0.7), "a TOPIC change is broadcast to the channel")
    b.send("TOPIC #top")
    got = b.read(1.0)
    check("332" in got and "the new topic" in got, "TOPIC query returns 332 and the topic")
    a.send("MODE #top +t"); a.read(0.5)
    b.send("TOPIC #top :bob hijacks")
    check("482" in b.read(1.0), "+t stops a non-operator changing the topic (482)")
    a.send("MODE #top -t"); a.read(0.5)
    b.send("TOPIC #top :now allowed")
    check("482" not in b.read(1.0), "-t lets a regular user change the topic")
    a.send("TOPIC #top :"); a.read(0.5); b.read(0.3)
    b.send("TOPIC #top")
    advisory("331" in b.read(1.0), "TOPIC with an empty trailing clears the topic",
             "RFC: `TOPIC #c :` unsets it, so a later query gives 331 not 332")
    a.close(); b.close()


def t_kick(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    a.send("JOIN #k"); a.read(); b.send("JOIN #k"); b.read(); a.read()
    b.send("KICK #k alice :try me")
    check("482" in b.read(1.0), "a regular user cannot KICK (482)")
    if srv.death():
        return
    a.send("KICK #k bob :out you go")
    check("KICK" in b.read(1.0), "the kicked user is notified")
    check("KICK" in a.read(0.5), "the operator sees the KICK too")
    b.send("PRIVMSG #k :am I still here?")
    time.sleep(0.4)
    check("still here" not in a.read(0.6), "the kicked user really left the channel")
    a.send("KICK #k ghost")
    got = a.read(0.7)
    check("401" in got or "441" in got, "KICK on a non-member returns 401/441")
    a.close(); b.close()


def t_mode_o(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    a.send("JOIN #o"); a.read(); b.send("JOIN #o"); b.read(); a.read()
    b.send("MODE #o +t")
    check("482" in b.read(1.0), "a regular user cannot change modes (482)")
    if srv.death():
        return
    a.send("MODE #o +o bob"); a.read(0.6); b.read(0.6)
    b.send("MODE #o +t")
    check("482" not in b.read(1.0), "+o grants operator rights")
    a.send("MODE #o -o bob"); a.read(0.6); b.read(0.6)
    b.send("MODE #o +i")
    check("482" in b.read(1.0), "-o removes operator rights")
    a.close(); b.close()


def t_mode_combined(srv):
    a = C(srv.port); a.register("alice", srv.password)
    a.send("JOIN #cmb"); a.read()
    a.send("MODE #cmb +itk thekey"); a.read(0.6)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    b.send("JOIN #cmb thekey")
    got = b.read(1.0)
    advisory("473" in got, "+itk applies every flag in the group (invite-only enforced)",
             f"expected 473 from +i; got {got[:120]!r} - combined flags carrying an "
             "argument are a common parsing bug")
    a.send("MODE #cmb -itk"); a.read(0.6)
    b.send("JOIN #cmb")
    advisory("JOIN" in b.read(1.0), "-itk removes every flag in the group")
    a.close(); b.close()


def t_mode_query(srv):
    a = C(srv.port); a.register("alice", srv.password)
    a.send("JOIN #q"); a.read()
    a.send("MODE #q +it"); a.read(0.6)
    a.send("MODE #q")
    got = a.read(1.0)
    advisory("324" in got, "MODE with no flags returns 324 RPL_CHANNELMODEIS",
             f"got {got[:120]!r} - IRC clients query this right after joining")
    a.close()


def t_part(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    a.send("JOIN #p"); a.read(); b.send("JOIN #p"); b.read(); a.read()
    b.send("PART #p :bye")
    check("PART" in a.read(1.0), "PART is broadcast to the remaining members")
    a.send("PRIVMSG #p :still there?")
    time.sleep(0.4)
    check("still there" not in b.read(0.6), "a parted user stops receiving channel messages")
    b.send("PART #nosuch")
    got = b.read(0.7)
    check("403" in got or "442" in got, "PART on an unknown channel returns 403")
    a.close(); b.close()


def t_multi_channel(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    for ch in ["#one", "#two", "#three"]:
        a.send(f"JOIN {ch}"); b.send(f"JOIN {ch}")
    a.read(1.2); b.read(1.2)
    a.send("PRIVMSG #two :only in two")
    got = b.read(1.0)
    check(got.count("only in two") == 1,
          "a client in three channels receives the message exactly once",
          f"arrived {got.count('only in two')} times: {got[:160]!r}")
    a.send("PART #two"); a.read(0.6); b.read(0.6)
    b.send("PRIVMSG #one :one still works")
    check("one still works" in a.read(1.0), "parting one channel does not affect the others")
    a.close(); b.close()


def t_op_errors(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.send("JOIN #e"); a.read()
    probes = [
        ("JOIN", "461"), ("JOIN #", None), ("JOIN nohash", "403"),
        ("KICK", "461"), ("KICK #e", "461"), ("KICK #nosuch bob", "403"),
        ("INVITE", "461"), ("INVITE bob", "461"), ("INVITE bob #nosuch", "403"),
        ("INVITE ghost #e", "401"),
        ("TOPIC", "461"), ("TOPIC #nosuch", "403"), ("TOPIC #e", "331"),
        ("MODE", "461"), ("MODE #e", None), ("MODE #nosuch +i", "403"),
        ("MODE #e +k", "461"), ("MODE #e +l", "461"), ("MODE #e +o", "461"),
        ("MODE #e +l abc", None), ("MODE #e +l -5", None), ("MODE #e +z", None),
        ("MODE #e +o ghost", "401|441"), ("MODE #e ++++", None),
        ("PART", "461"), ("PRIVMSG #nosuch :hi", "403"),
        ("KICK #e alice", None), ("PART #e", None), ("PART #e", "403|442"),
    ]
    for cmd, expect in probes:
        a.send(cmd)
        got = a.read(0.35)
        if srv.death():
            record(f'"{cmd}" does not kill the server', "CRASH", srv.death())
            return
        if expect:
            alts = expect.split("|")
            check(any(c in got for c in alts), f'"{cmd}" -> {" or ".join(alts)}',
                  repr(got[:140]) or "no reply")
        else:
            record(f'"{cmd}" handled without crashing', "PASS")
    a.send("PING :end")
    check("PONG" in a.read(0.8), "server responsive after every bad-argument probe")
    a.close()


# ───────────────────────── 7. RFC compliance ─────────────────────────

def t_case_commands(srv):
    a = C(srv.port)
    a.raw(f"pass {srv.password}\r\nnick lower\r\nuser lower 0 * :L\r\n")
    got = a.read(1.2)
    advisory("001" in got, "commands are case-insensitive (lowercase pass/nick/user)",
             f"got {got[:140]!r} - RFC 2812: command names are case-insensitive. "
             "Uppercase the command token before dispatch.")
    a.close()
    b = C(srv.port); b.register("mixed", srv.password); b.read()
    b.send("PiNg :mixedcase")
    advisory("PONG" in b.read(0.8), "mixed-case commands work (PiNg)")
    b.close()


def t_case_channels(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.send("JOIN #test"); a.read(0.8)
    b.send("JOIN #TEST"); b.read(0.8)
    a.read(0.5)
    a.send("PRIVMSG #test :same room?")
    advisory("same room?" in b.read(1.0),
             "channel names are case-insensitive (#test == #TEST)",
             "two evaluators joining #test and #Test land in different rooms and it "
             "looks broken. Normalise channel names for lookup.")
    a.close(); b.close()


def t_case_nicks(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password)
    a.read(); b.read()
    a.send("PRIVMSG BOB :wrong case")
    advisory("wrong case" in b.read(1.0), "nicknames are case-insensitive for PRIVMSG",
             "RFC 2812: nick comparison is case-insensitive")
    a.close(); b.close()


def t_comma_join(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.send("JOIN #x,#y")
    got = a.read(1.2)
    both = got.count("JOIN") >= 2 or ("#x" in got and "#y" in got and "#x,#y" not in got)
    advisory(both, "JOIN accepts a comma-separated channel list",
             f"got {got[:160]!r} - looks like one channel literally named '#x,#y'")
    a.send("PART #x,#y")
    got = a.read(1.0)
    advisory(got.count("PART") >= 2 or "403" not in got,
             "PART accepts a comma-separated channel list", repr(got[:160]))
    a.close()


def t_dup_join(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.send("JOIN #dup"); a.read(0.8)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    b.send("JOIN #dup"); b.read(0.8); a.read(0.5)
    a.send("JOIN #dup"); a.read(0.8)
    a.send("PRIVMSG #dup :once please")
    got = b.read(1.0)
    check(got.count("once please") == 1,
          "re-joining a channel does not duplicate the member",
          f"the message arrived {got.count('once please')} times")
    a.close(); b.close()


def t_ping_noparam(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.send("PING")
    got = a.read(0.8)
    advisory("409" in got, "PING with no parameter returns 409 ERR_NOORIGIN", repr(got[:140]))
    a.close()


# ───────────────────────── 8. IRC client compatibility ─────────────────────────

def t_cap(srv):
    a = C(srv.port)
    a.send("CAP LS 302")
    time.sleep(0.3)
    a.send(f"PASS {srv.password}")
    a.send("NICK ircclient")
    a.send("USER ircclient 0 * :Real Name")
    a.send("CAP END")
    got = a.read(1.5)
    check("001" in got, "registration completes when the client opens with CAP LS",
          f"got {got[:180]!r} - every modern IRC client starts this way")
    advisory("421" not in got, "CAP does not draw a 421 Unknown command",
             "harmless for most clients, but replying `CAP * LS :` and swallowing "
             "CAP END is cleaner")
    a.close()


def t_client_probes(srv):
    a = C(srv.port); a.register("alice", srv.password); a.read()
    a.send("JOIN #c"); a.read(0.8)
    for cmd, label in [("WHO #c", "WHO"), ("WHOIS alice", "WHOIS"),
                       ("MODE alice", "MODE <nick> (user-mode query)"),
                       ("NAMES #c", "NAMES"), ("LIST", "LIST"),
                       ("NOTICE #c :hi", "NOTICE")]:
        a.send(cmd)
        got = a.read(0.5)
        advisory("421" not in got and "403" not in got,
                 f"{label} is handled (clients send it automatically)",
                 f"got {got.strip()[:120]!r}")
    a.send("PING :end")
    check("PONG" in a.read(0.8), "server responsive after client-style probes")
    a.close()


def t_prefix(srv):
    a = C(srv.port); a.register("alice", srv.password)
    b = C(srv.port); b.register("bob", srv.password); b.read()
    a.send("JOIN #pfx"); a.read(0.8)
    b.send("JOIN #pfx"); b.read(0.8); a.read(0.5)
    a.send("PRIVMSG #pfx :check my prefix")
    got = b.read(1.0)
    m = re.search(r":(\S+)!(\S*)@(\S*) PRIVMSG", got)
    check(m is not None, "messages carry a nick!user@host prefix", repr(got[:160]))
    if m:
        advisory(m.group(3) != "", "the host part of the prefix is not empty",
                 f"prefix is {m.group(0)[:40]!r} - set the hostname in _acceptClient "
                 "(inet_ntoa on the accepted address, or 'localhost')")
        advisory(m.group(2) != "", "the user part of the prefix is not empty")
    a.close(); b.close()


# ───────────────────────── 9. shutdown and leaks ─────────────────────────

def signal_checks(binary, password, port):
    section("shutdown")
    srv = Server(binary, port, password)
    if not srv.start():
        record("server starts", "CRASH", srv.death() or "timeout")
        return
    a = C(port); a.register("alice", password); a.send("JOIN #s"); a.read(0.8)
    srv.proc.send_signal(signal.SIGINT)
    time.sleep(1.5)
    rc = srv.proc.poll()
    check(rc is not None, "SIGINT (Ctrl-C) stops the server", "still running after 1.5s")
    if rc is not None:
        check(rc >= 0, "SIGINT exit is graceful, not a signal death", f"rc={rc}")
    srv.stop()
    srv2 = Server(binary, port, password)
    check(srv2.start(), "the port is immediately reusable after shutdown (SO_REUSEADDR)",
          srv2.death() or "could not rebind")
    srv2.stop()


def leak_checks(binary, password, port):
    section("leaks")
    vg = shutil.which("valgrind")
    if not vg:
        record("valgrind leak check", "SKIP",
               "valgrind not installed - the sheet requires a leak check, so run "
               "`valgrind --leak-check=full ./ircserv <port> <pw>` by hand "
               "(or `leaks` on macOS)")
        return
    log = "/tmp/vg_irc.log"
    srv = Server(binary, port, password,
                 wrapper=[vg, "--leak-check=full", "--error-exitcode=0", f"--log-file={log}"])
    if not srv.start():
        record("server starts under valgrind", "FAIL", srv.death() or "timeout")
        return
    try:
        a = C(port); a.register("alice", password)
        b = C(port); b.register("bob", password)
        c = C(port); c.register("carol", password)
        for x in (a, b, c):
            x.send("JOIN #leak")
        time.sleep(0.8)
        a.send("PRIVMSG #leak :hello"); time.sleep(0.3)
        a.send("MODE #leak +itk key"); time.sleep(0.3)
        a.send("TOPIC #leak :a topic"); time.sleep(0.3)
        a.send("KICK #leak bob :out"); time.sleep(0.3)
        c.send("QUIT :bye"); time.sleep(0.3)
        a.kill_hard(); time.sleep(0.6)
    finally:
        srv.proc.send_signal(signal.SIGINT)
        time.sleep(4.0)
        if srv.proc.poll() is None:
            record("server exits on SIGINT under valgrind", "WARN", "had to kill it")
            srv.stop()
    text = ""
    try:
        text = open(log, errors="replace").read()
    except OSError:
        pass
    m = re.search(r"definitely lost:\s*([\d,]+) bytes", text)
    ind = re.search(r"indirectly lost:\s*([\d,]+) bytes", text)
    if not m:
        if "no leaks are possible" in text:
            record("no definitely/indirectly lost memory (0 bytes)", "PASS")
        else:
            record("valgrind leak summary parsed", "WARN", f"no summary in {log}")
        return
    lost = int(m.group(1).replace(",", "")) + (int(ind.group(1).replace(",", "")) if ind else 0)
    check(lost == 0, f"no definitely/indirectly lost memory ({lost} bytes)", f"see {log}")
    still = re.search(r"still reachable:\s*([\d,]+) bytes", text)
    if still:
        advisory(int(still.group(1).replace(",", "")) == 0,
                 f"nothing still reachable at exit ({still.group(1)} bytes)",
                 "42 usually tolerates still-reachable, but delete your Clients and "
                 "Channels in ~Server() to be safe")


# ───────────────────────── sections ─────────────────────────

SECTIONS = {
    "networking": [
        ("welcome burst", t_welcome),
        ("multiple clients", t_multi_clients),
        ("40 clients one channel", t_many_clients),
        ("PING/PONG", t_ping),
        ("channel broadcast", t_channel_broadcast),
        ("PRIVMSG", t_privmsg),
        ("PRIVMSG multi-target", t_multi_target),
        ("all interfaces", t_all_interfaces),
    ],
    "specials": [
        ("partial command", t_partial),
        ("one byte at a time", t_byte_at_a_time),
        ("many commands one packet", t_batched),
        ("bare LF endings", t_bare_lf),
        ("abruptly killed client", t_abrupt_kill),
        ("killed mid-command", t_half_command_kill),
        ("connect/disconnect churn", t_churn),
        ("flood a frozen client", t_frozen_flood),
        ("very long lines", t_long_line),
        ("junk input", t_junk),
    ],
    "registration": [
        ("wrong password", t_wrong_pass),
        ("no PASS", t_no_pass),
        ("USER before NICK", t_reg_order),
        ("nickname collision", t_nick_collision),
        ("invalid nicknames", t_bad_nick),
        ("nickname change", t_nick_change),
        ("re-registration", t_reregister),
        ("unknown command", t_unknown_cmd),
        ("QUIT", t_quit),
    ],
    "channel": [
        ("first joiner is operator", t_first_op),
        ("channel key +k", t_key),
        ("invite only +i", t_invite),
        ("user limit +l", t_limit),
        ("TOPIC and +t", t_topic),
        ("KICK", t_kick),
        ("operator flag +o", t_mode_o),
        ("combined mode flags", t_mode_combined),
        ("MODE query", t_mode_query),
        ("PART", t_part),
        ("several channels at once", t_multi_channel),
        ("bad arguments everywhere", t_op_errors),
    ],
    "rfc": [
        ("case-insensitive commands", t_case_commands),
        ("case-insensitive channels", t_case_channels),
        ("case-insensitive nicks", t_case_nicks),
        ("comma-separated lists", t_comma_join),
        ("duplicate JOIN", t_dup_join),
        ("PING with no parameter", t_ping_noparam),
    ],
    "client": [
        ("CAP negotiation", t_cap),
        ("commands clients send automatically", t_client_probes),
        ("well-formed prefixes", t_prefix),
    ],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./ircserv")
    ap.add_argument("--project", default=".")
    ap.add_argument("--src", default="src")
    ap.add_argument("--port", type=int, default=6767)
    ap.add_argument("--password", default="testpass")
    ap.add_argument("--only", default=None)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--no-static", action="store_true")
    ap.add_argument("--no-leaks", action="store_true")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if args.list:
        print("sections: " + ", ".join(["build", "static", "arguments"] +
                                       list(SECTIONS) + ["shutdown", "leaks"]))
        return

    only = args.only
    print(f"{B}ft_irc tester v2{N}  binary={args.binary} password={args.password}")

    if not args.no_build and only in (None, "build"):
        build_checks(args.project, args.binary)
    if not os.path.exists(args.binary):
        print(f"{R}{args.binary} not found - run make first{N}")
        sys.exit(1)
    if not args.no_static and only in (None, "static"):
        static_checks(args.src)
    if only in (None, "arguments"):
        arg_checks(args.binary, args.password)

    port = args.port
    for sec, tests in SECTIONS.items():
        if only and only != sec:
            continue
        section(sec)
        for name, fn in tests:
            group(Server(args.binary, port, args.password), name, fn)
            port += 1

    if only in (None, "shutdown"):
        signal_checks(args.binary, args.password, port); port += 1
    if not args.no_leaks and only in (None, "leaks"):
        leak_checks(args.binary, args.password, port)

    print(f"\n{B}=== summary ==={N}")
    counts = {}
    for _, _, st, _ in results:
        counts[st] = counts.get(st, 0) + 1
    for st in ("PASS", "FAIL", "CRASH", "WARN", "SKIP"):
        if counts.get(st):
            print(f"  {COLOR[st]}{st:5}{N} {counts[st]}")

    for label, want in (("CRASHES - each of these is a 0 at defense", "CRASH"),
                        ("FAILURES - fix before pushing", "FAIL"),
                        ("WARNINGS - will not fail you, but a grader may notice", "WARN")):
        hits = [(s, n) for s, n, st, _ in results if st == want]
        if hits:
            print(f"\n{COLOR[want]}{label}:{N}")
            for s, n in hits:
                print(f"  - [{s}] {n}")

    blocking = counts.get("FAIL", 0) + counts.get("CRASH", 0)
    if blocking == 0:
        print(f"\n{G}No blocking issues.{N} Two things this script cannot do for you:")
        print("  1. Connect with your reference IRC client (irssi / HexChat / WeeChat)")
        print("     and confirm no errors: /connect, /join, /msg, /kick, /mode, /part.")
        print("  2. Explain your poll() loop and disconnect handling out loud.")
    sys.exit(1 if blocking else 0)


if __name__ == "__main__":
    main()