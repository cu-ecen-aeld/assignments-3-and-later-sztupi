#include <arpa/inet.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

int terminated = 0;

void handle_signal(int sig) {
  if (sig == SIGTERM || sig == SIGINT) {
    terminated = 1;
  }
}

int main(int argc, char* argv[]) {
  int opt;
  int daemon = 0;
  while ((opt = getopt(argc, argv, "d")) != -1) {
    switch (opt) {
      case 'd':
        daemon = 1;
        break;
    }
  }

  openlog(NULL, 0, LOG_USER);

  signal(SIGTERM, handle_signal);
  signal(SIGINT, handle_signal);

  int socketfd = socket(PF_INET, SOCK_STREAM, 0);
  if (socketfd == -1) {
    syslog(LOG_ERR, "Error allocating socket: %s", strerror(errno));
    return -1;
  }

  struct addrinfo hints, *servinfo;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  int status = getaddrinfo(NULL, "9000", &hints, &servinfo);
  if (status != 0) {
    syslog(LOG_ERR, "Error getting addrinfo: %s", strerror(errno));
    return -1;
  }

  status = bind(socketfd, servinfo->ai_addr, servinfo->ai_addrlen);
  if (status != 0) {
    syslog(LOG_ERR, "Error binding socket to port: %s", strerror(errno));
    return -1;
  }
  syslog(LOG_DEBUG, "Bound to port");

  if (daemon) {
    int pid = fork();
    if (pid < 0) {
      syslog(LOG_ERR, "Error forking daemon process: %s", strerror(errno));
      return -1;
    } else if (pid > 0) {
      return 0;
    }
  }


  status = listen(socketfd, 5);
  if (status != 0) {
    syslog(LOG_ERR, "Error listening: %s", strerror(errno));
    return -1;
  }
  syslog(LOG_DEBUG, "Listening on port");

  struct sockaddr_storage addr;
  socklen_t addr_size;
  addr_size = sizeof(addr);

  while (!terminated) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socketfd, &read_fds);
    syslog(LOG_DEBUG, "Waiting for connection");
    int selectres = select(socketfd+1, &read_fds, NULL, NULL, NULL);
    if (selectres == -1) {
      if (errno == EINTR) {
        continue;
      } else {
        syslog(LOG_ERR, "Error waiting for connection: %s", strerror(errno));
        return -1;
      }
    }
    syslog(LOG_DEBUG, "Incoming connection");

    int connfd = accept(socketfd, (struct sockaddr *)&addr, &addr_size);
    if (connfd == -1) {
      syslog(LOG_ERR, "Error accepting connection: %s", strerror(errno));
      return -1;
    }
    syslog(LOG_INFO, "Accepted connection from %s",
           inet_ntoa(((struct sockaddr_in *)&addr)->sin_addr));

    ssize_t stream_size = 256;
    ssize_t stream_pos = 0;
    char *stream_buf = (char *)calloc(stream_size, sizeof(char));

    int stop = 0;
    while (!stop && !terminated) {
      FD_ZERO(&read_fds);
      FD_SET(connfd, &read_fds);
      selectres = select(connfd+1, &read_fds, NULL, NULL, NULL);
      if (selectres == -1) {
        if (errno == EINTR) {
          continue;
        } else {
          syslog(LOG_ERR, "Error waiting for recieve: %s", strerror(errno));
          return -1;
        }
      }

      char recv_buf[256];
      ssize_t recv_len = recv(connfd, (void *)recv_buf, 256, 0); // MSG_WAITALL?
      if (recv_len < 0) {
        syslog(LOG_ERR, "Error receiving: %s", strerror(errno));
      }
      if (recv_len == 0) {
        stop = 1;
      } else {
        if (stream_pos + recv_len >= stream_size) {
          stream_size = stream_size * 2;
          char *new_buf = (char *)calloc(stream_size, sizeof(char));
          strncpy(new_buf, stream_buf, stream_pos);
          free(stream_buf);
          stream_buf = new_buf;
        }
        strncpy(stream_buf+stream_pos, recv_buf, recv_len);
      }
      while (recv_len > 0) {
        stream_pos += 1;
        recv_len -= 1;
        if (stream_buf[stream_pos] == '\n') {
          int fdTarget = open("/var/tmp/aesdsocketdata", O_CREAT | O_APPEND | O_WRONLY, 0644);
          write(fdTarget, stream_buf, stream_pos+1);
          close(fdTarget);

          fdTarget = open("/var/tmp/aesdsocketdata", O_RDONLY);
          char send_buf[256];
          ssize_t send_len = read(fdTarget, send_buf, 256);
          if (send_len < 0) {
            syslog(LOG_ERR, "Error while reading from temp file: %s", strerror(errno));
            exit(-1);
          }
          while (send_len > 0) {
            char msg[257];
            strncpy(msg, send_buf, send_len);
            msg[send_len] = '\0';
            syslog(LOG_DEBUG, "Sending %s", msg);
            ssize_t total_sent = 0;
            while (total_sent < send_len) {
              ssize_t sent =
                  send(connfd, send_buf + total_sent, send_len - total_sent, 0);
              syslog(LOG_DEBUG, "Sent %d", (int)sent);
              total_sent += sent;
            }
            send_len = read(fdTarget, send_buf, 256);
            if (send_len < 0) {
              syslog(LOG_ERR, "Error while reading from temp file: %s", strerror(errno));
              exit(-1);
            }
          }
          close(fdTarget);

          strncpy(stream_buf, stream_buf+stream_pos+1, recv_len);
          stream_pos = -1;
        }
      }
    }

    free(stream_buf);

    syslog(LOG_INFO, "Closed connection from %s",
           inet_ntoa(((struct sockaddr_in *)&addr)->sin_addr));
  }

  syslog(LOG_INFO, "Caught signal, exiting");

  remove("/var/tmp/aesdsocketdata");
  if (socketfd) close(socketfd);
  freeaddrinfo(servinfo);

  return 0;
}
