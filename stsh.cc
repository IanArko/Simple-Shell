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

/**
 * Builtin Handlers
 * -----------------------
 */

static void fgHandler(const pipeline& p){
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
      break;
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
    }
    break;
  case 3: cout << "Called bg command\n"; break;
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
  int status;
  pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
  if (WIFEXITED(status) || WIFSIGNALED(status)) updateJobList(joblist, pid, kTerminated);
  if (WIFSTOPPED(status)) updateJobList(joblist, pid, kStopped);
  if (WIFCONTINUED(status)) updateJobList(joblist, pid, kRunning);
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
  STSHJob& job = joblist.addJob(kForeground);
  
  pid_t child = fork();
   
  if (child != 0){
    job.addProcess(STSHProcess(child, p.commands[0]));
  } else {
    int n = sizeof(p.commands[0].tokens) / sizeof(p.commands[0].tokens[0]);
    char *combined[n + 1];
    combined[0] = (char *)p.commands[0].command;
    
    for (int i=1; i < (n + 1); i++) {
        combined[i] = (char *)p.commands[0].tokens[i-1];
    }

    execvp(combined[0], combined);
    exit(0);
  }
  

  // stop the program to run what is in the foreground
  // Block all signals except for sigchild
  sigset_t mask;
  sigemptyset(&mask);
  // sigprocmask ????
  while(joblist.hasForegroundJob()) {
    sigsuspend(&mask);
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
