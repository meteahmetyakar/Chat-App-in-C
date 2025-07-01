# \# 📌 Multi-Threaded Chat \& File-Sharing System

# 

# A TCP-based, POSIX-threaded chat server and client supporting room management, private messaging, and file transfers with robust synchronization and logging.

# 

# ---

# 

# \## 📖 Project Overview

# 

# This project implements:

# \- A central \*\*chatserver\*\* handling up to \*\*256 concurrent\*\* clients.

# \- Client commands for \*\*joining/leaving rooms\*\*, \*\*broadcasting\*\*, \*\*whispering\*\*, and \*\*file transfers\*\* (≤ 3 MB, `.txt`/`.pdf`/`.jpg`/`.png`).

# \- \*\*File uploads\*\* are enqueued in a bounded ring buffer (capacity 5) and processed by dedicated worker threads.

# \- Graceful \*\*SIGINT\*\* shutdown notifying clients, cleaning up resources, and writing a timestamped log in `logs/YYYYMMDD\_HHMMSS.log`.

# 

# ---

# 

# \## 🏗️ Architecture

# 

# !\[Architecture Diagram](docs/architecture.png)

# 

# 1\. \*\*chatserver\*\* listens for new TCP connections.

# 2\. For each client, spawns a \*\*client\_handler\*\* thread (socket + username handshake).

# 3\. \*\*Client threads\*\* parse commands (`/join`, `/broadcast`, `/whisper`, `/sendfile`, `/leave`) and enqueue file uploads.

# 4\. A pool of \*\*file\_upload\_worker\*\* threads dequeues file items, locates recipients, and streams file data.

# 5\. Thread-safe \*\*rooms\*\*, \*\*connections\*\*, and \*\*file\_queue\*\* modules coordinate shared state with mutexes and condition variables.

# 

# ---

# 

# \## ⚙️ Installation

# 

# ```bash

# \# Clone the repository

# git clone https://github.com/meteahmetyakar/final-chat-project.git

# cd final-chat-project

# 

# \# Build server and client

# make

# ```

# 

# \*(Requires GCC, pthreads)\*

# 

# ---

# 

# \## ▶️ Usage

# 

# 1\. \*\*Start the server\*\* (port 5000):

# &nbsp;  ```bash

# &nbsp;  ./chatserver 5000

# &nbsp;  ```

# 

# 2\. \*\*Run clients\*\* (connect to server at 127.0.0.1:5000):

# &nbsp;  ```bash

# &nbsp;  ./chatclient 127.0.0.1 5000

# &nbsp;  ```

# 

# 3\. \*\*Client commands\*\*:

# &nbsp;  - `/username <name>` — Set unique 1–16 alphanumeric username.

# &nbsp;  - `/join <room\_name>` — Join or create a room (1–32 alphanumeric).

# &nbsp;  - `/broadcast <message>` — Send to all in current room.

# &nbsp;  - `/whisper <user> <message>` — Private message.

# &nbsp;  - `/sendfile <user> <path>` — Transfer a file (≤ 3 MB).

# &nbsp;  - `/leave` — Leave current room.

# &nbsp;  - `/exit` — Disconnect from server.

# 

# ---

# 

# \## 📂 File Structure

# 

# ```

# .

# ├── client/

# │   ├── chatclient.c          # Client main and recv\_thread

# │   ├── termios\_input.c/h     # Raw-mode prompt handler

# │   └── Makefile

# ├── server/

# │   ├── chatserver.c          # Server main and handlers

# │   ├── room.c/h              # Room management

# │   ├── conn\_helpers.c/h      # Connection lookup \& broadcasting

# │   ├── file\_queue.c/h        # Bounded ring buffer for file uploads

# │   ├── file\_upload\_worker.c  # Worker pool for file transfers

# │   └── log.c/h               # Thread-safe timestamped logging

# ├── docs/

# │   ├── architecture.png      # Architecture diagram

# │   └── screenshots/

# │       ├── username\_creation.png

# │       ├── messaging.png

# │       ├── leave.png

# │       └── sendfile.png

# ├── logs/                     # Auto-generated logs on SIGINT

# ├── Makefile                  # Build rules for server \& client

# └── README.md

# ```

# 

# ---

# 

# \## 🖼️ Runtime Screenshots

# 

# \### Username Creation

# 

# !\[Username Creation](docs/screenshots/username\_creation.png)

# 

# \### Private \& Room Messaging

# 

# !\[Messaging](docs/screenshots/messaging.png)

# 

# \### Leaving a Room

# 

# !\[Leave](docs/screenshots/leave.png)

# 

# \### File Transfer

# 

# !\[Sendfile](docs/screenshots/sendfile.png)

# 

# ---

# 

# \## 📜 Conclusion

# 

# This project demonstrates a fully concurrent, thread-safe chat system with advanced features, reinforcing skills in POSIX sockets, mutexes, condition variables, and producer–consumer patterns for file handling.

# 

# ---

# 

# \*\*Author:\*\* Mete Ahmet Yakar  

# \*\*Date:\*\* May 2025



