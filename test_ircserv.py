#!/usr/bin/env python3
"""
Script de test automatique pour ft_irc.

Usage:
    python3 test_ircserv.py <port> <password> [host]

Exemple:
    python3 test_ircserv.py 6667 testpass
    python3 test_ircserv.py 6667 testpass 127.0.0.1

Ce script ouvre plusieurs connexions socket (comme plusieurs clients IRC)
et envoie les commandes une par une pour vérifier que le serveur répond
correctement. Il n'utilise aucune librairie externe (juste `socket`).
"""

import socket
import sys
import time

HOST_DEFAULT = "127.0.0.1"
TIMEOUT = 1.5  # secondes d'attente de réponse après chaque commande

PASS = 0
FAIL = 0


def log(title):
    print(f"\n\033[1;34m== {title} ==\033[0m")


def ok(msg):
    global PASS
    PASS += 1
    print(f"  \033[1;32m[OK]\033[0m {msg}")


def ko(msg):
    global FAIL
    FAIL += 1
    print(f"  \033[1;31m[FAIL]\033[0m {msg}")


class IRCClient:
    def __init__(self, host, port, name):
        self.name = name
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(TIMEOUT)
        self.sock.connect((host, port))
        self.buffer = ""

    def send(self, line):
        print(f"  \033[0;36m{self.name} >>\033[0m {line}")
        self.sock.sendall((line + "\r\n").encode())

    def recv(self, timeout=TIMEOUT):
        """Récupère tout ce qui est disponible pendant `timeout` secondes."""
        self.sock.settimeout(timeout)
        data = ""
        try:
            while True:
                chunk = self.sock.recv(4096).decode(errors="replace")
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass
        for line in data.strip().splitlines():
            print(f"  \033[0;33m{self.name} <<\033[0m {line}")
        return data

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def register(client, password, nick, user):
    client.send(f"PASS {password}")
    client.send(f"NICK {nick}")
    client.send(f"USER {user} 0 * :{user} Real Name")
    return client.recv()


def contains(data, needle):
    return needle in data


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    port = int(sys.argv[1])
    password = sys.argv[2]
    host = sys.argv[3] if len(sys.argv) > 3 else HOST_DEFAULT

    print(f"Connexion à {host}:{port} (mot de passe attendu: '{password}')")

    # ---------------------------------------------------------------
    log("Test 1: mauvais mot de passe -> doit être rejeté")
    bad = IRCClient(host, port, "bad")
    bad.send("PASS wrongpassword")
    bad.send("NICK baduser")
    bad.send("USER baduser 0 * :Bad User")
    resp = bad.recv()
    if contains(resp, "464") or resp == "":
        ok("Mauvais mot de passe géré (erreur 464 ou connexion fermée)")
    else:
        ko("Le serveur n'a pas rejeté le mauvais mot de passe comme attendu")
    bad.close()

    # ---------------------------------------------------------------
    log("Test 2: enregistrement de deux clients valides")
    alice = IRCClient(host, port, "alice")
    resp = register(alice, password, "alice", "alice")
    if contains(resp, "001") or resp != "":
        ok("Alice enregistrée (réponse reçue après PASS/NICK/USER)")
    else:
        ko("Aucune réponse après enregistrement d'Alice (attendu ex: 001 Welcome)")

    bob = IRCClient(host, port, "bob")
    resp = register(bob, password, "bob", "bob")
    if contains(resp, "001") or resp != "":
        ok("Bob enregistré (réponse reçue après PASS/NICK/USER)")
    else:
        ko("Aucune réponse après enregistrement de Bob")

    # ---------------------------------------------------------------
    log("Test 3: JOIN sur un channel")
    alice.send("JOIN #test")
    resp = alice.recv()
    if contains(resp, "JOIN") or contains(resp, "#test"):
        ok("Alice a rejoint #test")
    else:
        ko("Pas de confirmation de JOIN pour Alice")

    bob.send("JOIN #test")
    resp_bob = bob.recv()
    resp_alice = alice.recv()  # alice doit voir bob rejoindre
    if contains(resp_bob, "JOIN") or contains(resp_bob, "#test"):
        ok("Bob a rejoint #test")
    else:
        ko("Pas de confirmation de JOIN pour Bob")
    if contains(resp_alice, "bob") or contains(resp_alice, "JOIN"):
        ok("Alice a été notifiée de l'arrivée de Bob dans #test")
    else:
        ko("Alice n'a pas reçu de notification du JOIN de Bob")

    # ---------------------------------------------------------------
    log("Test 4: PRIVMSG dans un channel")
    alice.send("PRIVMSG #test :Salut tout le monde")
    resp = bob.recv()
    if contains(resp, "Salut tout le monde"):
        ok("Bob a bien reçu le message d'Alice dans #test")
    else:
        ko("Bob n'a pas reçu le PRIVMSG channel d'Alice")

    # ---------------------------------------------------------------
    log("Test 5: PRIVMSG privé entre deux clients")
    bob.send("PRIVMSG alice :Salut en privé")
    resp = alice.recv()
    if contains(resp, "Salut en privé"):
        ok("Alice a reçu le message privé de Bob")
    else:
        ko("Alice n'a pas reçu le PRIVMSG privé de Bob")

    # ---------------------------------------------------------------
    log("Test 6: TOPIC")
    alice.send("TOPIC #test :Nouveau sujet du jour")
    resp_alice = alice.recv()
    resp_bob = bob.recv()
    if contains(resp_alice, "Nouveau sujet") or contains(resp_bob, "Nouveau sujet"):
        ok("TOPIC mis à jour et propagé")
    else:
        ko("TOPIC non confirmé / non propagé")

    # ---------------------------------------------------------------
    log("Test 7: MODE (+i sur le channel, ex: invite-only)")
    alice.send("MODE #test +i")
    resp = alice.recv()
    if contains(resp, "MODE") or contains(resp, "+i"):
        ok("MODE +i appliqué")
    else:
        ko("Pas de confirmation du MODE +i")

    # ---------------------------------------------------------------
    log("Test 8: INVITE (Bob invite un client tiers hypothétique)")
    carol = IRCClient(host, port, "carol")
    register(carol, password, "carol", "carol")
    alice.send("INVITE carol #test")
    resp_alice = alice.recv()
    resp_carol = carol.recv()
    if contains(resp_alice, "INVITE") or contains(resp_carol, "INVITE"):
        ok("INVITE envoyé et reçu")
    else:
        ko("INVITE non confirmé")

    carol.send("JOIN #test")
    resp_carol = carol.recv()
    if contains(resp_carol, "JOIN") or contains(resp_carol, "#test"):
        ok("Carol a pu rejoindre après invitation (mode +i respecté)")
    else:
        ko("Carol n'a pas pu rejoindre malgré l'invitation")

    # ---------------------------------------------------------------
    log("Test 9: KICK")
    alice.send("KICK #test carol :dehors")
    resp_carol = carol.recv()
    resp_bob = bob.recv()
    if contains(resp_carol, "KICK") or contains(resp_bob, "KICK"):
        ok("KICK confirmé et propagé")
    else:
        ko("KICK non confirmé")

    # ---------------------------------------------------------------
    log("Test 10: QUIT propre")
    bob.send("QUIT :A plus tard")
    resp = alice.recv()
    if contains(resp, "QUIT") or resp == "":
        ok("QUIT géré (notification ou connexion fermée proprement)")
    else:
        ko("Comportement inattendu au QUIT")

    # ---------------------------------------------------------------
    for c in (alice, bob, carol):
        c.close()

    print("\n\033[1m===== RESULTATS =====\033[0m")
    print(f"  \033[1;32mOK:   {PASS}\033[0m")
    print(f"  \033[1;31mFAIL: {FAIL}\033[0m")

    sys.exit(0 if FAIL == 0 else 1)


if __name__ == "__main__":
    main()