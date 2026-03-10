#pragma once

// Cross-platform socket compatibility layer.
// Provides unified macros for socket operations on Windows and POSIX.

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define FD_SETSIZE 1024
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  using SocketHandle = SOCKET;
  using socklen_t = int;
  #define SOCK_CLOSE(s)   closesocket(s)
  #define SOCK_INVALID    INVALID_SOCKET
  #define SOCK_ERR        SOCKET_ERROR
  #define SOCK_ERRNO      WSAGetLastError()
  #define SOCK_WOULDBLOCK WSAEWOULDBLOCK
  #define SOCK_INPROGRESS WSAEWOULDBLOCK
  #define SOCK_SETNONBLOCK(s) do { u_long nb_ = 1; ioctlsocket((s), FIONBIO, &nb_); } while(0)
  #define SOCK_SELECT_NFDS 0
  #define SOCK_SLEEP_MS(ms) Sleep(ms)
  #define SOCK_CAST(p) reinterpret_cast<char*>(p)
  #define SOCK_CCAST(p) reinterpret_cast<const char*>(p)
  #pragma comment(lib, "ws2_32.lib")
  #pragma comment(lib, "iphlpapi.lib")
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <poll.h>
  #include <errno.h>
  #include <time.h>
  using SocketHandle = int;
  #define SOCK_CLOSE(s)   close(s)
  #define SOCK_INVALID    (-1)
  #define SOCK_ERR        (-1)
  #define SOCK_ERRNO      errno
  #define SOCK_WOULDBLOCK EWOULDBLOCK
  #define SOCK_INPROGRESS EINPROGRESS
  #define SOCK_SETNONBLOCK(s) do { int fl_ = fcntl((s), F_GETFL, 0); fcntl((s), F_SETFL, fl_ | O_NONBLOCK); } while(0)
  #define SOCK_SELECT_NFDS(fds) ((int)(fds) + 1)
  #define SOCK_SLEEP_MS(ms) usleep((ms) * 1000)
  #define SOCK_CAST(p) reinterpret_cast<char*>(p)
  #define SOCK_CCAST(p) reinterpret_cast<const char*>(p)
#endif

// Windows SDK <iprtrmib.h> (via <iphlpapi.h>) defines IP_STATS, ICMP_STATS,
// TCP_STATS, UDP_STATS, IP6_STATS as enum values.  lwIP opt.h redefines them
// as preprocessor macros.  Undefine the Windows versions to avoid C4005.
#undef IP_STATS
#undef ICMP_STATS
#undef TCP_STATS
#undef UDP_STATS
#undef IP6_STATS
