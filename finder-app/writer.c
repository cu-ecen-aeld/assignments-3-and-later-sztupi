#include <stdio.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc == 1) {
    fprintf(stderr, "argument <writefile> missing\n");
    return 1;
  } else if (argc == 2) {
    fprintf(stderr, "argument <writestr> missing\n");
    return 1;
  }
  const char * writefile = argv[1];
  const char * writestr = argv[2];

  openlog(NULL, 0, LOG_USER);
  syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

  int fdTarget = creat(writefile, 0644);
  if (fdTarget == -1) {
    const char * errstr = strerror(errno);
    syslog(LOG_ERR, "Error opening file %s for writing: %s", writefile, errstr);
    return 1;
  }

  int err = write(fdTarget, writestr, strlen(writestr));
  if (err == -1) {
    const char * errstr = strerror(errno);
    syslog(LOG_ERR, "Error writing to file %s: %s", writefile, errstr);
    return 1;
  }

  err = close(fdTarget);
  if (err == -1) {
    const char * errstr = strerror(errno);
    syslog(LOG_ERR, "Error closing file %s: %s", writefile, errstr);
    return 1;
  }

  return 0;
}
