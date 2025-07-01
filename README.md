# 📌 Multi-Threaded Chat & File-Sharing System

A TCP-based, POSIX-threaded chat server and client supporting room management, private messaging, and file transfers with robust synchronization and logging.

---

## 📖 Project Overview

This project implements:
- A central **chatserver** handling up to **256 concurrent** clients.
- Client commands for **joining/leaving rooms**, **broadcasting**, **whispering**, and **file transfers** (≤ 3 MB, `.txt`/`.pdf`/`.jpg`/`.png`).
- **File uploads** are enqueued in a bounded ring buffer (capacity 5) and processed by dedicated worker threads.
- Graceful **SIGINT** shutdown notifying clients, cleaning up resources, and writing a timestamped log in `logs/YYYYMMDD_HHMMSS.log`.

---

## 🏗️ Architecture

<img src="https://github.com/meteahmetyakar/Chat-App-in-C/blob/main/images/architecture-diagram.png"/>

1. **chatserver** listens for new TCP connections.
2. For each client, spawns a **client_handler** thread (socket + username handshake).
3. **Client threads** parse commands (`/join`, `/broadcast`, `/whisper`, `/sendfile`, `/leave`) and enqueue file uploads.
4. A pool of **file_upload_worker** threads dequeues file items, locates recipients, and streams file data.
5. Thread-safe **rooms**, **connections**, and **file_queue** modules coordinate shared state with mutexes and condition variables.

---

## ⚙️ Installation

```bash
# Clone the repository
git clone https://github.com/meteahmetyakar/Chat-App-in-C.git
cd Chat-App-in-C

# Build server and client
make all
```

*(Requires GCC, pthreads)*

---

## ▶️ Usage

1. **Start the server** (port 5000):
   ```bash
   ./chatserver 5000
   ```

2. **Run clients** (connect to server at 127.0.0.1:5000):
   ```bash
   ./chatclient 127.0.0.1 5000
   ```

3. **Client commands**:
   - `/username <name>` — Set unique 1–16 alphanumeric username.
   - `/join <room_name>` — Join or create a room (1–32 alphanumeric).
   - `/broadcast <message>` — Send to all in current room.
   - `/whisper <user> <message>` — Private message.
   - `/sendfile <user> <path>` — Transfer a file (≤ 3 MB).
   - `/leave` — Leave current room.
   - `/exit` — Disconnect from server.

---

## 📜 Conclusion

This project demonstrates a fully concurrent, thread-safe chat system with advanced features, reinforcing skills in POSIX sockets, mutexes, condition variables, and producer–consumer patterns for file handling.

---
