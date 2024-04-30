#include "systemcalls.h"
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <syslog.h>
#include <sys/file.h>


/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{

/*
 * TODO  add your code here
 *  Call the system() function with the command set in the cmd
 *   and return a boolean true if the system() call completed with success
 *   or false() if it returned a failure
*/
    int result = system(cmd);

    return result == 0;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/

bool do_exec(int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

/*
 * TODO:
 *   Execute a system command by calling fork, execv(),
 *   and wait instead of system (see LSP page 161).
 *   Use the command[0] as the full path to the command to execute
 *   (first argument to execv), and use the remaining arguments
 *   as second argument to the execv() command.
 *
*/

    int pid = fork();
    if (pid == 0) {
        for (int i = 0; i<count; i++){
            syslog(LOG_DEBUG, "child process args %d: %s", i, command[i]);
        }
        int result = execv(command[0], command);
        if (result == -1) {
            int err = errno;
            const char * errstr = strerror(err);
            syslog(LOG_ERR, "child process failed to start: %s", errstr);
            exit(-1);
        }
        // will never reach it, here only to satisfy C branch checker
        exit(0);
    } else {
        int wstatus;
        pid_t resultPid = waitpid(pid, &wstatus, 0);
        if (resultPid == -1) {
            int waitErr = errno;
            const char * errstr = strerror(waitErr);
            syslog(LOG_ERR, "child process execution failed: %s", errstr);
            return false;
        }
        if (WIFEXITED(wstatus)) {
            syslog(LOG_DEBUG, "child process exited with status %d", WEXITSTATUS(wstatus));
            return WEXITSTATUS(wstatus) == 0;
        } else {
            syslog(LOG_DEBUG, "child process did not exit properly");
            return false;
        }
    }

    va_end(args);
}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;
    // this line is to avoid a compile warning before your implementation is complete
    // and may be removed

/*
 * TODO
 *   Call execv, but first using https://stackoverflow.com/a/13784315/1446624 as a refernce,
 *   redirect standard out to a file specified by outputfile.
 *   The rest of the behaviour is same as do_exec()
 *
*/
    int fd = creat(outputfile, 0644);
    if (fd < 0) {
        int err = errno;
        const char * errstr = strerror(err);
        syslog(LOG_ERR, "cannot create output file %s: %s", outputfile, errstr);
        return false;
    }

    int pid = fork();
    if (pid == 0) {
        if (dup2(fd, 1) < 0) {
            int err = errno;
            const char * errstr = strerror(err);
            syslog(LOG_ERR, "child process failed to duplicate output file handle: %s", errstr);
            return false;
        }
        if (execv(command[0], command) == -1) {
            int err = errno;
            const char * errstr = strerror(err);
            syslog(LOG_ERR, "child process failed to start: %s", errstr);
            return false;
        }
        // will never reach this, here only to satisfy C branch checker
        return true;
    } else {
        close(fd);

        int wstatus;
        if (waitpid(pid, &wstatus, 0) == -1) {
            int waitErr = errno;
            const char * errstr = strerror(waitErr);
            syslog(LOG_ERR, "child process execution failed: %s", errstr);
            return false;
        }
        if (WIFEXITED(wstatus)) {
            return (WEXITSTATUS(wstatus)) == 0;
        } else {
            return false;
        }
    }

    va_end(args);

    return true;
}
