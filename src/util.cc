#define __STDC_FORMAT_MACROS
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <event2/util.h>
#include <glog/logging.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <poll.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>

#include <string>
#include <algorithm>

#include "util.h"
#include "status.h"

#ifndef POLLIN
# define POLLIN      0x0001    /* There is data to read */
# define POLLPRI     0x0002    /* There is urgent data to read */
# define POLLOUT     0x0004    /* Writing now will not block */
# define POLLERR     0x0008    /* Error condition */
# define POLLHUP     0x0010    /* Hung up */
# define POLLNVAL    0x0020    /* Invalid request: fd not open */
#endif

#define AE_READABLE 1
#define AE_WRITABLE 2

namespace Util {
sockaddr_in NewSockaddrInet(const std::string &host, uint32_t port) {
  sockaddr_in sin{};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = inet_addr(host.c_str());
  sin.sin_port = htons(port);
  return sin;
}

Status SockConnect(std::string host, uint32_t port, int *fd, uint64_t conn_timeout, uint64_t timeout) {
  sockaddr_in sin{};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = inet_addr(host.c_str());
  sin.sin_port = htons(port);
  *fd = socket(AF_INET, SOCK_STREAM, 0);
  if (conn_timeout > 0) {
    fcntl(*fd, F_SETFL, O_NONBLOCK);
    connect(*fd, reinterpret_cast<sockaddr *>(&sin), sizeof(sin));

    if ((Util::aeWait(*fd, AE_WRITABLE, conn_timeout) & AE_WRITABLE) == 0) {
      close(*fd);
      *fd = -1;
      return Status(Status::NotOK, strerror(errno));
    }

    int socket_arg;
    // Set to blocking mode again...
    if ((socket_arg = fcntl(*fd, F_GETFL, NULL)) < 0) {
      close(*fd);
      *fd = -1;
      return Status(Status::NotOK, strerror(errno));
    }
    socket_arg &= (~O_NONBLOCK);
    if (fcntl(*fd, F_SETFL, socket_arg) < 0) {
      close(*fd);
      *fd = -1;
      return Status(Status::NotOK, strerror(errno));
    }
  } else {
    auto rv = connect(*fd, reinterpret_cast<sockaddr *>(&sin), sizeof(sin));
    if (rv < 0) {
      close(*fd);
      *fd = -1;
      return Status(Status::NotOK, strerror(errno));
    }
  }

  if (timeout > 0) {
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    if (setsockopt(*fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&tv), sizeof(tv)) < 0) {
      close(*fd);
      *fd = -1;
      return Status(Status::NotOK, "setsockopt failed");
    }
  }

  setsockopt(*fd, SOL_SOCKET, SO_KEEPALIVE, nullptr, 0);
  setsockopt(*fd, IPPROTO_TCP, TCP_NODELAY, nullptr, 0);
  return Status::OK();
}

// NOTE: fd should be blocking here
Status SockSend(int fd, const std::string &data) {
  ssize_t n = 0;
  while (n < static_cast<ssize_t>(data.size())) {
    ssize_t nwritten = write(fd, data.c_str()+n, data.size()-n);
    if (nwritten == -1) {
      return Status(Status::NotOK, strerror(errno));
    }
    n += nwritten;
  }
  return Status::OK();
}

int GetPeerAddr(int fd, std::string *addr, uint32_t *port) {
  sockaddr_storage sa{};
  socklen_t sa_len = sizeof(sa);
  if (getpeername(fd, reinterpret_cast<sockaddr *>(&sa), &sa_len) < 0) {
    return -1;
  }
  if (sa.ss_family == AF_INET) {
    auto sa4 = reinterpret_cast<sockaddr_in *>(&sa);
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, reinterpret_cast<void *>(&sa4->sin_addr), buf, INET_ADDRSTRLEN);
    addr->clear();
    addr->append(buf);
    *port = ntohs(sa4->sin_port);
    return 0;
  }
  return -2;  // only support AF_INET currently
}

Status StringToNum(const std::string &str, int64_t *n, int64_t min, int64_t max) {
  try {
    *n = static_cast<int64_t>(std::stoll(str));
    if (max > min && (*n < min || *n > max)) {
      return Status(Status::NotOK, "value shoud between "+std::to_string(min)+" and "+std::to_string(max));
    }
  } catch (std::exception &e) {
    return Status(Status::NotOK, "value is not an integer or out of range");
  }
  return Status::OK();
}

std::string ToLower(std::string in) {
  std::transform(in.begin(), in.end(), in.begin(),
                 [](char c) -> char { return static_cast<char>(std::tolower(c)); });
  return in;
}

void Trim(const std::string &in, const std::string &chars, std::string *out) {
  out->clear();
  if (in.empty()) return;
  out->assign(in);
  out->erase(0, out->find_first_not_of(chars));
  out->erase(out->find_last_not_of(chars)+1);
}

void Split(std::string in, std::string delim, std::vector<std::string> *out) {
  if (in.empty() || !out) return;
  out->clear();

  std::string::size_type pos = 0;
  std::string elem, trimed_elem;
  do {
    pos = in.find_first_of(delim);
    elem = in.substr(0, pos);
    Trim(elem, delim, &trimed_elem);
    if (!trimed_elem.empty()) out->push_back(trimed_elem);
    in = in.substr(pos+1);
  } while (pos != std::string::npos);
}

int StringMatch(const std::string &pattern, const std::string &in, int nocase) {
  return StringMatchLen(pattern.c_str(), pattern.length(), in.c_str(), in.length(), nocase);
}

// Glob-style pattern matching.
int StringMatchLen(const char *pattern, int patternLen,
                   const char *string, int stringLen, int nocase) {
  while (patternLen && stringLen) {
    switch (pattern[0]) {
      case '*':
        while (pattern[1] == '*') {
          pattern++;
          patternLen--;
        }
        if (patternLen == 1)
          return 1; /* match */
        while (stringLen) {
          if (StringMatchLen(pattern + 1, patternLen - 1,
                             string, stringLen, nocase))
            return 1; /* match */
          string++;
          stringLen--;
        }
        return 0; /* no match */
        break;
      case '?':
        if (stringLen == 0)
          return 0; /* no match */
        string++;
        stringLen--;
        break;
      case '[': {
        int not_symbol, match;

        pattern++;
        patternLen--;
        not_symbol = pattern[0] == '^';
        if (not_symbol) {
          pattern++;
          patternLen--;
        }
        match = 0;
        while (1) {
          if (pattern[0] == '\\' && patternLen >= 2) {
            pattern++;
            patternLen--;
            if (pattern[0] == string[0])
              match = 1;
          } else if (pattern[0] == ']') {
            break;
          } else if (patternLen == 0) {
            pattern--;
            patternLen++;
            break;
          } else if (pattern[1] == '-' && patternLen >= 3) {
            int start = pattern[0];
            int end = pattern[2];
            int c = string[0];
            if (start > end) {
              int t = start;
              start = end;
              end = t;
            }
            if (nocase) {
              start = tolower(start);
              end = tolower(end);
              c = tolower(c);
            }
            pattern += 2;
            patternLen -= 2;
            if (c >= start && c <= end)
              match = 1;
          } else {
            if (!nocase) {
              if (pattern[0] == string[0])
                match = 1;
            } else {
              if (tolower(static_cast<int>(pattern[0])) == tolower(static_cast<int>(string[0])))
                match = 1;
            }
          }
          pattern++;
          patternLen--;
        }
        if (not_symbol)
          match = !match;
        if (!match)
          return 0; /* no match */
        string++;
        stringLen--;
        break;
      }
      case '\\':
        if (patternLen >= 2) {
          pattern++;
          patternLen--;
        }
        /* fall through */
      default:
        if (!nocase) {
          if (pattern[0] != string[0])
            return 0; /* no match */
        } else {
          if (tolower(static_cast<int>(pattern[0])) != tolower(static_cast<int>(string[0])))
            return 0; /* no match */
        }
        string++;
        stringLen--;
        break;
    }
    pattern++;
    patternLen--;
    if (stringLen == 0) {
      while (*pattern == '*') {
        pattern++;
        patternLen--;
      }
      break;
    }
  }
  if (patternLen == 0 && stringLen == 0)
    return 1;
  return 0;
}

void BytesToHuman(char *buf, size_t size, uint64_t n) {
  double d;

  if (n < 1024) {
    snprintf(buf, size, "%" PRIu64 "B", n);
  } else if (n < (1024*1024)) {
    d = static_cast<double>(n)/(1024);
    snprintf(buf, size, "%.2fK", d);
  } else if (n < (1024LL*1024*1024)) {
    d = static_cast<double>(n)/(1024*1024);
    snprintf(buf, size, "%.2fM", d);
  } else if (n < (1024LL*1024*1024*1024)) {
    d = static_cast<double>(n)/(1024LL*1024*1024);
    snprintf(buf, size, "%.2fG", d);
  } else if (n < (1024LL*1024*1024*1024*1024)) {
    d = static_cast<double>(n)/(1024LL*1024*1024*1024);
    snprintf(buf, size, "%.2fT", d);
  } else if (n < (1024LL*1024*1024*1024*1024*1024)) {
    d = static_cast<double>(n)/(1024LL*1024*1024*1024*1024);
    snprintf(buf, size, "%.2fP", d);
  } else {
    snprintf(buf, size, "%" PRIu64 "B", n);
  }
}

bool IsPortInUse(int port) {
  int fd;
  Status s = SockConnect("0.0.0.0", static_cast<uint32_t>(port), &fd);
  if (fd > 0) close(fd);
  return s.IsOK();
}

void ThreadSetName(const char *name) {
#ifdef __APPLE__
  pthread_setname_np(name);
#else
  pthread_setname_np(pthread_self(), name);
#endif
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
int aeWait(int fd, int mask, uint64_t timeout) {
  struct pollfd pfd;
  int retmask = 0, retval;

  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = fd;
  if (mask & AE_READABLE) pfd.events |= POLLIN;
  if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

  if ((retval = poll(&pfd, 1, timeout)) == 1) {
    if (pfd.revents & POLLIN) retmask |= AE_READABLE;
    if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
    if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
    if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
    return retmask;
  } else {
    return retval;
  }
}

}  // namespace Util
