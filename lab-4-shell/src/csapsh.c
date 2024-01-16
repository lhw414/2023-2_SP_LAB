//--------------------------------------------------------------------------------------------------
// Shell Lab                                 Fall 2023                           System Programming
//
/// @file
/// @brief csapsh - a tiny shell with job control
/// @author hyunwoo LEE
/// @studid 2020-12907
///
/// @section changelog Change Log
/// 2020/11/14 Bernhard Egger adapted from CS:APP lab
/// 2021/11/03 Bernhard Egger improved for 2021 class
///
/// @section license_section License
/// Copyright CS:APP authors
/// Copyright (c) 2020-2023, Computer Systems and Platforms Laboratory, SNU
/// All rights reserved.
///
/// Redistribution and use in source and binary forms, with or without modification, are permitted
/// provided that the following conditions are met:
///
/// - Redistributions of source code must retain the above copyright notice, this list of condi-
///   tions and the following disclaimer.
/// - Redistributions in binary form must reproduce the above copyright notice, this list of condi-
///   tions and the following disclaimer in the documentation and/or other materials provided with
///   the distribution.
///
/// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
/// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED  TO, THE IMPLIED  WARRANTIES OF MERCHANTABILITY
/// AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
/// CONTRIBUTORS  BE LIABLE FOR ANY DIRECT,  INDIRECT, INCIDENTAL, SPECIAL,  EXEMPLARY,  OR CONSE-
/// QUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
/// LOSS OF USE, DATA,  OR PROFITS; OR BUSINESS INTERRUPTION)  HOWEVER CAUSED AND ON ANY THEORY OF
/// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
/// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
/// DAMAGE.
//--------------------------------------------------------------------------------------------------

#define _GNU_SOURCE          // to get basename() in string.h
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "jobcontrol.h"
#include "parser.h"

//--------------------------------------------------------------------------------------------------
// Global variables
//

char prompt[] = "csapsh> ";  ///< command line prompt (DO NOT CHANGE)
int emit_prompt = 1;         ///< 1: emit prompt; 0: do not emit prompt
int verbose = 0;             ///< 1: verbose mode; 0: normal mode


//--------------------------------------------------------------------------------------------------
// Functions that you need to implement
//
// Refer to the detailed descriptions at each function implementation.

void eval(char *cmdline);
int  builtin_cmd(char *argv[]);
void do_bgfg(char *argv[]);
void waitfg(int jid);

void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);


//--------------------------------------------------------------------------------------------------
// Implemented functions - do not modify
//

// main & helper functions
int main(int argc, char **argv);
void usage(const char *program);
void unix_error(char *msg);
void app_error(char *msg);
void Signal(int signum, void (*handler)(int));
void sigquit_handler(int sig);
char* stripnewline(char *str);

#define VERBOSE(...)  { if (verbose) { fprintf(stderr, ##__VA_ARGS__); fprintf(stderr, "\n"); } }





/// @brief Program entry point.
int main(int argc, char **argv)
{
  char c;
  char cmdline[MAXLINE];

  // redirect stderr to stdout so that the driver will get all output on the pipe connected 
  // to stdout.
  dup2(STDOUT_FILENO, STDERR_FILENO);

  // set Standard I/O's buffering mode for stdout and stderr to line buffering
  // to avoid any discrepancies between running the shell interactively or via the driver
  setlinebuf(stdout);
  setlinebuf(stderr);

  // parse command line
  while ((c = getopt(argc, argv, "hvp")) != EOF) {
    switch (c) {
      case 'h': usage(argv[0]);        // print help message
                break;
      case 'v': verbose = 1;           // emit additional diagnostic info
                break;
      case 'p': emit_prompt = 0;       // don't print a prompt
                break;                 // handy for automatic testing
      default:  usage(argv[0]);        // invalid option -> print help message
    }
  }

  // install signal handlers
  VERBOSE("Installing signal handlers...");
  Signal(SIGINT,  sigint_handler);     // Ctrl-c
  Signal(SIGTSTP, sigtstp_handler);    // Ctrl-z
  Signal(SIGCHLD, sigchld_handler);    // Terminated or stopped child
  Signal(SIGQUIT, sigquit_handler);    // Ctrl-Backslash (useful to exit shell)

  // execute read/eval loop
  VERBOSE("Execute read/eval loop...");
  while (1) {
    if (emit_prompt) { printf("%s", prompt); fflush(stdout); }

    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin)) {
      app_error("fgets error");
    }

    if (feof(stdin)) break;            // end of input (Ctrl-d)

    eval(cmdline);

    fflush(stdout);
  }

  // that's all, folks!
  return EXIT_SUCCESS;
}



/// @brief Evaluate the command line. The function @a parse_cmdline() does the heavy lifting of 
///        parsing the command line and splitting it into separate char *argv[] arrays that 
///        represent individual commands with their arguments. 
///        A job consists of one process or several processes connected via pipes. Optionally,
///        the output of the entire job can be saved into a file specified by outfile.
///        The shell waits for jobs that are executed in the foreground, while jobs that run
///        in the background are not waited for.
///        To allow piping of built-in commands, the shell has to fork itself before executing a
///        built-in command. This requires special treatement of the standalone "quit" command
///        that must not be executed in a forked shell to have the desired effect.
/// @param cmdline command line

void eval(char *cmdline)
{
  #define P_READ  0                      // pipe read end
  #define P_WRITE 1                      // pipe write end

  char *str = strdup(cmdline);
  VERBOSE("eval(%s)", stripnewline(str));
  free(str);

  char ***argv  = NULL;
  char *infile  = NULL;
  char *outfile = NULL;
  JobState mode;

  // parse command line
  int ncmd = parse_cmdline(cmdline, &mode, &argv, &infile, &outfile);
  VERBOSE("parse_cmdline(...) = %d", ncmd);
  if (ncmd == -1) return;              // parse error
  if (ncmd == 0)  return;              // no input
  assert(ncmd > 0);

  // dump parsed command line
  if (verbose) dump_cmdstruct(argv, infile, outfile, mode);

  // if the command is a single built-in command (no pipes or redirection), do not fork. Instead,
  // execute the command directly in this process. Note that this is not just to be more efficient -
  // it is necessary for the 'quit' command to work.
  if ((ncmd == 1) && (outfile == NULL)) {
    if (builtin_cmd(argv[0])) {
      free_cmdstruct(argv);
      return;
    }
  }

  // Block sigchld (temporally)
  sigset_t sgset, prev_one;
  sigemptyset(&sgset);
  sigaddset(&sgset, SIGCHLD);
  sigprocmask(SIG_BLOCK, &sgset, &prev_one);

  // Init process id and job id
  pid_t pid;
  int jid = -1;

  // Init array to hold the pipes
  int pipes[ncmd - 1][2];

  // Init pid array
  pid_t *pid_array;
  pid_array = malloc(sizeof(pid_t) * ncmd);

  // Piping the pipes array
  for (int i = 0; i < ncmd - 1; i++) {
    if (pipe(pipes[i]) == -1) { // when pipe error occurs, print unix error
      unix_error("Pipe error");
      exit(1);
    }
  }

  // Fork process and run cmd in each process
  for (int i=0; i < ncmd; ++i) {
    // When current process is child process
    if ((pid = fork()) == 0) {
      sigprocmask(SIG_SETMASK, &prev_one, NULL);
      // Store current pid in pid array
      pid_array[i] = getpid();
      // Set process group id as first process id 
      setpgid(getpid(), pid_array[0]);
      // For first cmd, check infile redirection
      if (i == 0 && infile != NULL) {
        // Open file and return file descriptor
        int fd = open(infile, O_RDONLY, 0);
        // When file descriptor error occurs, print error message
        if (fd == -1) {
          printf("Could not open file %s for input redirection\n", infile);
          exit(1);
        }
        // When dup2 error occurs, printf error message
        if (dup2(fd, STDIN_FILENO) == -1) {
          unix_error("dup2 input error");
          exit(1);
        }
        // Close file descriptor
        close(fd);
        if (i > 0) {
          close(pipes[i - 1][P_READ]);
        }
      } else if (i != 0) {
        VERBOSE("input piping"); // For detail mode
        // When dup2 error occurs while duplicating pipe and input stream, print error message
        if (dup2(pipes[i - 1][P_READ], STDIN_FILENO) == -1) {
          unix_error("dup2 output error");
          exit(1);
        }
        // Close pipe
        close(pipes[i - 1][P_READ]);
      }
      // For last cmd, check outfile redirection
      if (i == ncmd - 1 && outfile != NULL) {
        VERBOSE("outfile redirection");
        int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        // When file descriptor error occurs, print error message
        if (fd == -1) { 
          unix_error("open error");
          exit(1);
        }
        // When dup2 error occurs, printf error message
        if (dup2(fd, STDOUT_FILENO) == -1) {
          unix_error("dup2 error");
          exit(1);
        }
        // Close file descriptor
        close(fd);
        close(pipes[i][P_WRITE]);
      } else if (i < ncmd - 1) {
        VERBOSE("output piping"); // For detail mode
        // When dup2 error occurs while duplicating pipe and output stream, print error message
        if (dup2(pipes[i][P_WRITE], STDOUT_FILENO) == -1) {
          unix_error("dup2 error");
          exit(1);
        }
        // Close pipe
        close(pipes[i][P_WRITE]);
      }
      // Close unused pipe
      for (int j = 0; j < ncmd - 1; j++) {
        if (j != i - 1) close(pipes[j][P_READ]);
        if (j != i) close(pipes[j][P_WRITE]);
      }
      // Execute command, and error checking
      if (execvp(argv[i][0], argv[i]) < 0) {
        printf("No such file or directory\n");
        exit(0);
      }
    } else if (pid < 0) { // when fork error occurs, print error message
      unix_error("fork error");
    } else { // when current process is parent process
      // Close pipes in parent process
      if (i > 0) close(pipes[i - 1][P_READ]);
      if (i < ncmd - 1) close(pipes[i][P_WRITE]);
      // Store pid in pid array
      pid_array[i] = pid;
      if (i == ncmd - 1) {
        // Unblock SIGCHLD after last child is forked
        sigprocmask(SIG_SETMASK, &prev_one, NULL);
      }
    }
  }
  // When current process is parent process
  if (pid != 0) {
    if (mode == jsForeground) { // When job is Foreground
      // Add job in job list
      jid = addjob(pid_array[0], pid_array, ncmd, jsForeground, cmdline);
      waitfg(jid);
    } else { // When job is background
      // Add job in job list
      jid = addjob(pid_array[0], pid_array, ncmd, jsBackground, cmdline);
      printjob(jid);
    }
  }

}


/// @brief Execute built-in commands
/// @param argv command
/// @retval 1 if the command was a built-in command
/// @retval 0 otherwise
int builtin_cmd(char *argv[])
{
  VERBOSE("builtin_cmd(%s)", argv[0]);
  if      (strcmp(argv[0], "quit") == 0)  exit(EXIT_SUCCESS); // when command is quit
  else if (strcmp(argv[0], "fg") == 0)    do_bgfg(argv);      // when command is fg
  else if (strcmp(argv[0], "bg") == 0)    do_bgfg(argv);      // when command is bg
  else if (strcmp(argv[0], "jobs") == 0)  listjobs();         // when command is jobs
  else return 0; // when other commands, just return 0

  return 1; // when command is successfully excuted, return 1
}

/// @brief Execute the builtin bg and fg commands
/// @param argv char* argv[] array where 
///           argv[0] is either "bg" or "fg"
///           argv[1] is either a job id "%<n>", a process group id "@<n>" or a process id "<n>"
void do_bgfg(char *argv[])
{
  VERBOSE("do_bgfg(%s, %s)", argv[0], argv[1]);
  // When there's no argv[1], print error message
  if (argv[1] == NULL) {
    printf("%s command requires PID or %%jobid argument\n", argv[0]);
    return;
  }

  Job *job;
  // Get job using jid, pgid, or pid
  if (argv[1][0] == '%') { // when argument is job id
    // Get job id
    int job_jid = atoi(&argv[1][1]);
    // If there's no job, print error message
    if ((job = getjob_jid(job_jid)) == NULL) {
      printf("[%s] No such job\n", argv[1]);
      return;
    }
  } else if (argv[1][0] == '@') { // when argument is process group id
    // Get job process group id
    pid_t job_pgid = atoi(&argv[1][1]);
    // If there's no job, print error message
    if ((job = getjob_pgid(job_pgid)) == NULL) {
      printf("(%s) No such process group\n", argv[1]);
      return;
    }
  } else { // when argument is process id
    // Get job process id
    pid_t job_pid = atoi(&argv[1][0]);
    // If there's no job, print error message
    if ((job = getjob_pid(job_pid)) == NULL) {
      printf("{%d} No such process\n", job_pid);
      return;
    }
  }
  // Let job continue
  if(kill(-1 * job->pgid, SIGCONT) == -1) {
    unix_error("ERROR: Fail to SIGCONT");
  }
  // Switch job state
  if (strcmp(argv[0], "fg") == 0) { // when fg command
    // Switch to jsForeground, and waitfg
    job->state = jsForeground;
    waitfg(job->jid);
  } else { // when bg command
    // Switch to jsBackground, and print job
    job->state = jsBackground;
    printjob(job->jid);
  }
}

/// @brief Block until job jid is no longer in the foreground
/// @param jid job ID of foreground job
void waitfg(int jid)
{ 
  VERBOSE("Waitfg"); // for detail mode
  // Get jod using job id
  Job *job = getjob_jid(jid);
  // Check job is not null and job state is jsForeground, if not reurn
  while((job != NULL) && job->state == jsForeground) {
    sleep(1);
    // Renew job
    job = getjob_jid(jid);
  }
}


//--------------------------------------------------------------------------------------------------
// Signal handlers
//

/// @brief SIGCHLD handler. Sent to the shell whenever a child process terminates or stops because
///        it received a SIGSTOP or SIGTSTP signal. This handler reaps all zombies.
/// @param sig signal (SIGCHLD)
void sigchld_handler(int sig)
{
  VERBOSE("[SCH] SIGCHLD handler (signal: %d)", sig); // for detail mode
  // Store old errno and mask
  int olderrno = errno;
  sigset_t mask_all, prev_all;
  // Init pid and status
  pid_t pid;
  int status;
  // Block signals
  sigfillset(&mask_all);
  sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
  // Check child process status, and reaping zombie process
  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
    // Get job
    Job *job = getjob_pid(pid);
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
      // Delete job in job list
      if (job != NULL) {
        job->nproc_cur--;
        if (job->nproc_cur == 0) {
          deletejob(job->jid);
        }
      }
    } else if (WIFSTOPPED(status)) { // for stopped job
      if (job != NULL) { // make job state jsStopped
        job->state = jsStopped;
      }
    }
  }
  // Roll back signal mask and errno
  sigprocmask(SIG_SETMASK, &prev_all, NULL);
  errno = olderrno;
}

/// @brief SIGINT handler. Sent to the shell whenever the user types Ctrl-c at the keyboard.
///        Forward the signal to the foreground job.
/// @param sig signal (SIGINT)
void sigint_handler(int sig)
{
  VERBOSE("[SIH] SIGINT handler (signal: %d)", sig); // for detail mode
  // Store old errno and mask
  int olderrno = errno;
  sigset_t mask_all, prev_all;
  // Block signals
  sigfillset(&mask_all);
  sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
  // Get foreground job
  Job *fgjob = getjob_foreground();
  // If foreground job is not null, kill process group
  if (fgjob != NULL) {
    kill(-fgjob->pgid, sig);
  }
  // Roll back signal mask and errno
  sigprocmask(SIG_SETMASK, &prev_all, NULL);
  errno = olderrno;

}

/// @brief SIGTSTP handler. Sent to the shell whenever the user types Ctrl-z at the keyboard.
///        Forward the signal to the foreground job.
/// @param sig signal (SIGTSTP)
void sigtstp_handler(int sig)
{
  VERBOSE("[SSH] SIGTSTP handler (signal: %d)", sig);
  // Store old errno and mask
  int olderrno = errno;
  sigset_t mask_all, prev_all;
  // Block signals
  sigfillset(&mask_all);
  sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
  // Get foreground job
  Job *fgjob = getjob_foreground();
  // If foreground job is not null, kill process group
  if (fgjob != NULL) {
    fgjob->state = jsStopped;
    kill(-fgjob->pgid, SIGTSTP);
  }
  // Roll back signal mask and errno
  sigprocmask(SIG_SETMASK, &prev_all, NULL);
  errno = olderrno;
}


//--------------------------------------------------------------------------------------------------
// Other helper functions
//

/// @brief Print help message. Does not return.
__attribute__((noreturn))
void usage(const char *program)
{
  printf("Usage: %s [-hvp]\n", basename(program));
  printf("   -h   print this message\n");
  printf("   -v   print additional diagnostic information\n");
  printf("   -p   do not emit a command prompt\n");
  exit(EXIT_FAILURE);
}

/// @brief Print a Unix-level error message based on errno. Does not return.
/// param msg additional descriptive string (optional)
__attribute__((noreturn))
void unix_error(char *msg)
{
  if (msg != NULL) fprintf(stdout, "%s: ", msg);
  fprintf(stdout, "%s\n", strerror(errno));
  exit(EXIT_FAILURE);
}

/// @brief Print an application-level error message. Does not return.
/// @param msg error message
__attribute__((noreturn))
void app_error(char *msg)
{
  fprintf(stdout, "%s\n", msg);
  exit(EXIT_FAILURE);
}

/// @brief Wrapper for sigaction(). Installs the function @a handler as the signal handler
///        for signal @a signum. Does not return on error.
/// @param signum signal number to catch
/// @param handler signal handler to invoke
void Signal(int signum, void (*handler)(int))
{
  struct sigaction action;

  action.sa_handler = handler;
  sigemptyset(&action.sa_mask); // block sigs of type being handled
  action.sa_flags = SA_RESTART; // restart syscalls if possible

  if (sigaction(signum, &action, NULL) < 0) unix_error("Sigaction");
}

/// @brief SIGQUIT handler. Terminates the shell.
__attribute__((noreturn))
void sigquit_handler(int sig)
{
  printf("Terminating after receipt of SIGQUIT signal\n");
  exit(EXIT_SUCCESS);
}

/// @brief strip newlines (\n) from a string. Warning: modifies the string itself!
///        Inside the string, newlines are replaced with a space, at the end 
///        of the string, the newline is deleted.
///
/// @param str string
/// @reval char* stripped string
char* stripnewline(char *str)
{
  char *p = str;
  while (*p != '\0') {
    if (*p == '\n') *p = *(p+1) == '\0' ? '\0' : ' ';
    p++;
  }

  return str;
}
