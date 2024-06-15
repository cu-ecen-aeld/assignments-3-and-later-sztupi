#include <arpa/inet.h>
#include <bits/time.h>
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
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/queue.h>

int terminated = 0;

const char * targetFile =
  #ifdef USE_AESD_CHAR_DEVICE
    "/dev/aesdchar"
  #else
    "/var/tmp/aesdsocketdata"
  #endif
  ;

void handle_signal(int sig) {
  if (sig == SIGTERM || sig == SIGINT) {
    terminated = 1;
  }
}

int listen_socket(int daemon, struct addrinfo **servinfo) {
  int socketfd = socket(PF_INET, SOCK_STREAM, 0);
  if (socketfd == -1) {
    syslog(LOG_ERR, "Error allocating socket: %s", strerror(errno));
    return -1;
  }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  int status = getaddrinfo(NULL, "9000", &hints, servinfo);
  if (status != 0) {
    syslog(LOG_ERR, "Error getting addrinfo: %s", strerror(errno));
    return -1;
  }

  status = bind(socketfd, (*servinfo)->ai_addr, (*servinfo)->ai_addrlen);
  if (status != 0) {
    syslog(LOG_ERR, "Error binding socket to port: %s", strerror(errno));
    return -1;
  }

  if (daemon) {
    int pid = fork();
    if (pid < 0) {
      syslog(LOG_ERR, "Error forking daemon process: %s", strerror(errno));
      exit(-1);
    } else if (pid > 0) {
      exit(0);
    }
  }

  status = listen(socketfd, 5);
  if (status != 0) {
    syslog(LOG_ERR, "Error listening: %s", strerror(errno));
    return -1;
  }

  return socketfd;
}

int wait_for_connection(int socketfd, struct sockaddr_storage * addr) {
  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(socketfd, &read_fds);
  int selectres = select(socketfd+1, &read_fds, NULL, NULL, NULL);
  if (selectres == -1) {
    if (errno == EINTR) {
      return 0;
    } else {
      syslog(LOG_ERR, "Error waiting for connection: %s", strerror(errno));
      return -1;
    }
  }

  socklen_t addr_size;
  addr_size = sizeof(&addr);

  int connfd = accept(socketfd, (struct sockaddr *)addr, &addr_size);
  if (connfd == -1) {
    syslog(LOG_ERR, "Error accepting connection: %s", strerror(errno));
    return -1;
  }
  syslog(LOG_INFO, "Accepted connection from %s",
          inet_ntoa(((struct sockaddr_in *)addr)->sin_addr));

  return connfd;
}

int wait_for_data(int connfd) {
  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(connfd, &read_fds);
  int selectres = select(connfd + 1, &read_fds, NULL, NULL, NULL);
  if (selectres == -1) {
    if (errno == EINTR) {
      return 0;
    } else {
      syslog(LOG_ERR, "Error waiting for recieve: %s", strerror(errno));
      return -1;
    }
  }
  return 1;
}

const int BUF_SIZE = 256;

struct stream_data {
  ssize_t size;
  ssize_t pos;
  ssize_t unprocessed;
  char *buf;
};

void stream_allocate(struct stream_data *stream) {
  stream->size = BUF_SIZE;
  stream->pos = 0;
  stream->buf = (char *)calloc(stream->size, sizeof(char));
  stream->unprocessed = 0;
}

void stream_free(struct stream_data *stream) {
  free(stream->buf);
}

void stream_extend_buffer(struct stream_data *stream) {
  stream->size = stream->size * 2;
  char *new_buf = (char *)calloc(stream->size, sizeof(char));
  strncpy(new_buf, stream->buf, stream->pos + stream->unprocessed);
  free(stream->buf);
  stream->buf = new_buf;
}

int stream_has_space(struct stream_data *stream, int len) {
  return stream->pos + stream->unprocessed + len < stream->size;
}

void stream_append_data(struct stream_data *stream, char* buf, int len) {
  while (!stream_has_space(stream, len)) stream_extend_buffer(stream);
  strncpy(stream->buf + stream->pos + stream->unprocessed, buf, len);
  stream->unprocessed += len;
}

int stream_receive(int connfd, struct stream_data *stream) {
  char recv_buf[BUF_SIZE];
  ssize_t recv_len = recv(connfd, (void *)recv_buf, BUF_SIZE, 0); // MSG_WAITALL?
  if (recv_len < 0) {
    syslog(LOG_ERR, "Error receiving: %s", strerror(errno));
  }
  if (recv_len == 0) {
    return 0;
  } else {
    stream_append_data(stream, recv_buf, recv_len);
  }
  return 1;
}

int stream_process(struct stream_data *stream) {
  while (stream->unprocessed > 0 && stream->buf[stream->pos] != '\n') {
    stream->unprocessed -= 1;
    stream->pos += 1;
  }
  return stream->buf[stream->pos] == '\n';
}

void stream_write(struct stream_data *stream, int fd) {
  write(fd, stream->buf, stream->pos+1);
  strncpy(stream->buf, stream->buf + stream->pos+1, stream->unprocessed);
  stream->pos = 0;
}

void send_data(int fdTarget, int fdSource) {
  char send_buf[256];
  ssize_t send_len = read(fdSource, send_buf, 256);
  if (send_len < 0) {
    syslog(LOG_ERR, "Error while reading from temp file: %s", strerror(errno));
    exit(-1);
  }
  while (send_len > 0) {
    char msg[257];
    strncpy(msg, send_buf, send_len);
    msg[send_len] = '\0';
    ssize_t total_sent = 0;
    while (total_sent < send_len) {
      ssize_t sent =
          send(fdTarget, send_buf + total_sent, send_len - total_sent, 0);
      total_sent += sent;
    }
    send_len = read(fdSource, send_buf, 256);
    if (send_len < 0) {
      syslog(LOG_ERR, "Error while reading from temp file: %s",
             strerror(errno));
      exit(-1);
    }
  }
}

typedef struct {
  int connfd;
  int done;
  struct sockaddr_storage conn_addr;
  pthread_mutex_t *mutex;
} thread_data_t;

typedef struct tl_entry {
  pthread_t thread;
  thread_data_t tdata;
  SLIST_ENTRY(tl_entry) list;
} tl_entry_t;

SLIST_HEAD(tl_list, tl_entry);

void * th_listen(void * arg) {
  thread_data_t *data = (thread_data_t *)arg;

  struct stream_data stream;
  stream_allocate(&stream);

  int client_eof = 0;
  while (!client_eof && !terminated) {
    int status = wait_for_data(data->connfd);
    if (status == -1)
      exit(-1);
    else if (status == 0)
      continue;

    status = stream_receive(data->connfd, &stream);
    if (status == 0)
      client_eof = 1;

    while (stream_process(&stream)) {
      pthread_mutex_lock(data->mutex);
      int fdtarget =
          open(targetFile, O_CREAT | O_APPEND | O_WRONLY, 0644);
      stream_write(&stream, fdtarget);
      fsync(fdtarget);
      close(fdtarget);

      int fddata = open(targetFile, O_RDONLY);
      send_data(data->connfd, fddata);
      close(fddata);
      pthread_mutex_unlock(data->mutex);
    }
  }

  stream_free(&stream);

  syslog(LOG_INFO, "Closed connection from %s",
         inet_ntoa(((struct sockaddr_in *)&(data->conn_addr))->sin_addr));

  return NULL;
}

typedef struct {
  pthread_mutex_t *mutex;
} timer_th_data_t;

void * th_timer(void * arg) {
  timer_th_data_t *data = (timer_th_data_t *)arg;

  struct timespec t;
  int ret = clock_gettime(CLOCK_MONOTONIC, &t);
  if (ret) { perror("clock_gettime"); exit(-1); }

  t.tv_sec += 10;

  while (!terminated) {
    ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    if (ret == EINTR) {
      if (terminated) break; else continue;
    }
    t.tv_sec += 10;

    pthread_mutex_lock(data->mutex);
    int fdtarget = open(targetFile, O_CREAT | O_APPEND | O_WRONLY, 0644);
    if (fdtarget < 0) {
      syslog(LOG_ERR, "timer couldn't open file");
      perror("open");
      exit(-1);
    }

    #ifdef USE_AESD_CHAR_DEVICE
    time_t now = time(NULL);
    struct tm *nowtm = gmtime(&now);
    char outstr[200];
    int size = strftime(outstr, sizeof(outstr)-1, "%a, %d %b %Y %T %z", nowtm);
    outstr[size-1] = '\n';

    const char prefix[] = "timestamp:";
    write(fdtarget, prefix, sizeof(prefix)-1);
    write(fdtarget, outstr, size);
    #endif

    close(fdtarget);

    pthread_mutex_unlock(data->mutex);
  }
  return NULL;
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

  struct addrinfo *servinfo;
  int socketfd = listen_socket(daemon, &servinfo);
  if (socketfd == -1) exit(-1);

  struct tl_list tl_list;
  SLIST_INIT(&tl_list);

  pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

  timer_th_data_t timer_data;
  timer_data.mutex = &file_mutex;
  pthread_t timer;
  pthread_create(&(timer), NULL, &th_timer, (void *)&timer_data);

  while (!terminated) {
    struct sockaddr_storage conn_addr;
    int connfd = wait_for_connection(socketfd, &conn_addr);
    if (connfd == -1)
      exit(-1);
    else if (connfd == 0)
      continue;

    tl_entry_t *listener = (tl_entry_t *)malloc(sizeof(tl_entry_t));

    listener->tdata.connfd = connfd;
    listener->tdata.conn_addr = conn_addr;
    listener->tdata.done = 0;
    listener->tdata.mutex = &file_mutex;

    pthread_create(&(listener->thread), NULL, &th_listen, (void *)&(listener->tdata));
    SLIST_INSERT_HEAD(&tl_list, listener, list);

    tl_entry_t * entry = SLIST_FIRST(&tl_list);
    while (entry != NULL && entry->tdata.done) {
      SLIST_REMOVE_HEAD(&tl_list, list);
      pthread_join(entry->thread, NULL);
      free(entry);
      entry = SLIST_FIRST(&tl_list);
    }
    while (entry != NULL) {
      tl_entry_t * next = SLIST_NEXT(entry, list);
      while (next != NULL && next->tdata.done) {
        SLIST_REMOVE(&tl_list, next, tl_entry, list);
        pthread_join(next->thread, NULL);
        free(next);
        next = SLIST_NEXT(entry, list);
      }
      entry = next;
    }
  }

  syslog(LOG_INFO, "Caught signal, exiting");

  pthread_kill(timer, SIGINT);
  pthread_join(timer, NULL);

  #ifndef USE_AESD_CHAR_DEVICE
  remove(targetFile);
  #endif
  if (socketfd) close(socketfd);
  freeaddrinfo(servinfo);

  return 0;
}
