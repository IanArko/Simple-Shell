/**
 * File: stsh.cc
 * -------------
 * Defines the entry point of the stsh executable.
 */

#include "stsh-parser/stsh-parse.h"
#include "stsh-parser/stsh-readline.h"
#include "stsh-parser/stsh-parse-exception.h"
#include "stsh-signal.h"
#include "stsh-job-list.h"
#include "stsh-job.h"
#include "stsh-process.h"
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <algorithm>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>  // for fork
#include <signal.h>  // for kill
#include <sys/wait.h>
#include "fork-utils.h" // this needs to be the last #include in the list
using namespace std;

static STSHJobList joblist; // the one piece of global data we need so signal handlers can access it

static void waitForFg(){
  // stop the program to run what is in the foreground
  // Block all signals except for sigchild
  sigset_t mask;
  sigemptyset(&mask);
  // sigaddset(&mask, SIGCHLD);
  // Link below contains a sample run that may or may not be an issue. If it isn't delete these lines.
  // https://www.notion.so/Maybe-Timing-Error-e5f15d7fb6c74829baba2c76113e3dec
  // sigprocmask ????
  // cout << "WAIT FOR FG" << endl;
  // cout << joblist;
  while(joblist.hasForegroundJob()) {
    sigsuspend(&mask);
  }
}

/**
 * Builtin Handlers
 * -----------------------
 */

static void fgHandler(const pipeline& p){
  // Get the inputs and do error checking
  char* token0 = p.commands[0].tokens[0];
  char* token1 = p.commands[0].tokens[1];
  
  int t0 = 0;
  if (token0 != NULL) {  
    t0 = atoi(token0);
  }
  if(t0 < 1 || token1 != NULL){
    throw STSHException("Usage: fg <jobid>.");
  } else {
    if(!joblist.containsJob(t0)){
      throw STSHException("fg " + to_string(t0) + ": No such job.");
    } else {
      // IF there were no errors, get the job with #t0 from the job list
      STSHJob& job = joblist.getJob(t0);
      pid_t groupID = job.getGroupID();
      kill(groupID, SIGCONT);      
      job.setState(kForeground);
      /*
      if (tcsetpgrp(STDIN_FILENO, groupID) < 0) {
          throw STSHException("Failed to transfer STDIN control to foreground process.");
      }
      */
      waitForFg();
    }    
  }
}

static void bgHandler(const pipeline& p){
  // Get the inputs and do error checking
  char* token0 = p.commands[0].tokens[0];
  char* token1 = p.commands[0].tokens[1];

  int t0 = 0;
  if (token0 != NULL) {
    t0 = atoi(token0);
  }
  if(t0 < 1 || token1 != NULL){
    throw STSHException("Usage: bg <jobid>.");
  } else {
    if(!joblist.containsJob(t0)){
      throw STSHException("bg " + to_string(t0) + ": No such job.");
    } else {
      // IF there were no errors, get the job with #t0 from the job list
      STSHJob& job = joblist.getJob(t0);
      pid_t groupID = job.getGroupID();
      kill(groupID, SIGCONT);
      job.setState(kBackground);
    }
  }
}

/**
 * Function: handleBuiltin
 * -----------------------
 * Examines the leading command of the provided pipeline to see if
 * it's a shell builtin, and if so, handles and executes it.  handleBuiltin
 * returns true if the command is a builtin, and false otherwise.
 */
static const string kSupportedBuiltins[] = {"quit", "exit", "fg", "bg", "slay", "halt", "cont", "jobs"};
static const size_t kNumSupportedBuiltins = sizeof(kSupportedBuiltins)/sizeof(kSupportedBuiltins[0]);
static bool handleBuiltin(const pipeline& pipeline) {
  const string& command = pipeline.commands[0].command;
  auto iter = find(kSupportedBuiltins, kSupportedBuiltins + kNumSupportedBuiltins, command);
  if (iter == kSupportedBuiltins + kNumSupportedBuiltins) return false;
  size_t index = iter - kSupportedBuiltins;

  switch (index) {
  case 0:
  case 1: exit(0);
  case 2: // fg
    try {
      fgHandler(pipeline);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
    }
    break;
  case 3: // bg
    try {
      bgHandler(pipeline);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
    }
    break;
  case 4: cout << "Called slay command\n"; break;
  case 5: cout << "Called halt command\n"; break;
  case 6: cout << "Called cont command\n"; break;
  case 7: cout << joblist; break;
  default: throw STSHException("Internal Error: Builtin command not supported."); // or not implemented yet
  }
  
  return true;
}

static void updateJobList(STSHJobList& jobList, pid_t pid, STSHProcessState state) {
     if (!jobList.containsProcess(pid)) return;
     STSHJob& job = jobList.getJobWithProcess(pid);
     assert(job.containsProcess(pid));
     STSHProcess& process = job.getProcess(pid);
     process.setState(state);
     jobList.synchronize(job);
}

/* Our Signal Hanler Definitions
 * -------------------------------
 */

/* Function: sigChild
 * -------------------------------
 * sigChildReaps our child process and removes jobs from the job list
 */
static void sigChild(int sig){
  // cout << "SIG CHILD HANDLER" << endl;
  // cout << joblist;
  int status;
  pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
  if (WIFEXITED(status) || WIFSIGNALED(status)) {
     updateJobList(joblist, pid, kTerminated);
  } else if(WIFSTOPPED(status)) {
     updateJobList(joblist, pid, kStopped);
  } else { // WIFCONTINUED(status)
    updateJobList(joblist, pid, kRunning);
  }
  if (tcsetpgrp(STDIN_FILENO, getpid()) < 0) {
      throw STSHException("Failed to transfer STDIN control back to terminal.");
  }
  // cout << joblist;
}

/* Function: sigForward
 * -------------------------------
 * Pass a signal to the foreground process
 */
static void sigForward(int sig){
  if(joblist.hasForegroundJob()){
    kill(joblist.getForegroundJob().getGroupID(), sig);
  }
}


/**
 * Function: installSignalHandlers
 * -------------------------------
 * Installs user-defined signals handlers for four signals
 * (once you've implemented signal handlers for SIGCHLD, 
 * SIGINT, and SIGTSTP, you'll add more installSignalHandler calls) and 
 * ignores two others.
 *
 * installSignalHandler is a wrapper around a more robust version of the
 * signal function we've been using all quarter.  Check out stsh-signal.cc
 * to see how it works.
 */
static void installSignalHandlers() {
  // Our signal handlers
  installSignalHandler(SIGCHLD, sigChild);
  installSignalHandler(SIGINT, sigForward);
  installSignalHandler(SIGTSTP, sigForward);


  // Default signal handlers
  installSignalHandler(SIGQUIT, [](int sig) { exit(0); });
  installSignalHandler(SIGTTIN, SIG_IGN);
  installSignalHandler(SIGTTOU, SIG_IGN);
}

/**
 * Function: createJob
 * -------------------
 * Creates a new job on behalf of the provided pipeline.
 */
static void createJob(const pipeline& p) {
  int n = p.commands.size();
  int fds[(n-1)*2];
  pipe2(fds, O_CLOEXEC);
  STSHJob& job = joblist.addJob(kForeground);
  
  if(p.background) {
    job.setState(kBackground);
    cout << "[" << to_string(job.getNum()) << "] ";
  }

  // First Process  
  pid_t pid0 = fork();
  if (pid0 != 0){
      setpgid(pid0, 0);
    job.addProcess(STSHProcess(pid0, p.commands[0]));    
    // Print each of the group ids
    if(p.background) {
      cout << job.getGroupID();
    }
  } else {
    setpgid(getpid(), 0);
    dup2(fds[1],STDOUT_FILENO);

    int n = sizeof(p.commands[0].tokens) / sizeof(p.commands[0].tokens[0]);
    char *combined[n + 1];
    combined[0] = (char *)p.commands[0].command;
    
    for (int i=1; i < (n + 1); i++) {
        combined[i] = (char *)p.commands[0].tokens[i-1];
    }

    execvp(combined[0], combined);
    close(fds[0]);
    close(fds[1]);
    exit(0);
  }
  
  // Middle proccesses  


  // Last Process
  if (n > 1){ 
  pid_t pidl = fork();

  if (pidl != 0){
    setpgid(pidl, pid0);
    job.addProcess(STSHProcess(pidl, p.commands[1]));

    // Print each of the group ids
    if(p.background) {
      cout << " " << pidl << endl;
    }
  } else {
    setpgid(getpid(), pid0);
    dup2(fds[0],STDIN_FILENO);

    char *combined[kMaxArguments + 1];
    combined[0] = (char *)p.commands[1].command;

    for (int i=1; i < (kMaxArguments + 1); i++) {
        combined[i] = (char *)p.commands[1].tokens[i-1];
    }

    execvp(combined[0], combined);
    close(fds[0]);
    close(fds[1]);
    exit(0);
  }
  }
  close(fds[0]);
  close(fds[1]);

  // Run fg proccess in fg
  if(!p.background) {
    /*
    if (tcsetpgrp(STDIN_FILENO, child) < 0) {
      throw STSHException("Failed to transfer STDIN control to foreground process.");
    }
    */
    waitForFg();
  }
}

/**
 * Function: main
  --------------
 * Defines the entry point for a process running stsh.
 * The main function is little more than a read-eval-print
 * loop (i.e. a repl).  
 */
int main(int argc, char *argv[]) {
  pid_t stshpid = getpid();
  installSignalHandlers();
  rlinit(argc, argv); // configures stsh-readline library so readline works properly
  while (true) {
    string line;
    if (!readline(line)) break;
    if (line.empty()) continue;
    try {
      pipeline p(line);
      bool builtin = handleBuiltin(p);
      if (!builtin) createJob(p);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
      if (getpid() != stshpid) exit(0); // if exception is thrown from child process, kill it
    }
  }

  return 0;
}
