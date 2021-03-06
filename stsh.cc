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
  while(joblist.hasForegroundJob()) {
    sigsuspend(&mask);
  }
}

/**
 * Builtin Handlers
 * -----------------------
 */
static void fgbgHandler(const pipeline& p, string builtin, int sig){
  // Get the inputs and do error checking
  char* token0 = p.commands[0].tokens[0];
  char* token1 = p.commands[0].tokens[1];
  
  int t0 = 0;
  if (token0 != NULL) {  
    t0 = atoi(token0);
  }
  if(t0 < 1 || token1 != NULL){
    throw STSHException("Usage: " + builtin + " <jobid>.");
  } else {
    if(!joblist.containsJob(t0)){
      throw STSHException(builtin + " " + to_string(t0) + ": No such job.");
    } else {
      // IF there were no errors, get the job with #t0 from the job list
      STSHJob& job = joblist.getJob(t0);
      pid_t groupID = job.getGroupID();
      kill(-groupID, sig);      
      job.setState(kForeground);
    
      if (builtin == "fg") {
        if (tcsetpgrp(STDIN_FILENO, groupID) < 0) {
          throw STSHException("Failed to transfer STDIN control to foreground process.");
        } 
        waitForFg();
      }    
    }
  }
}

static void singleProcessHandler(const pipeline& p, string builtin, int sig){
  // Get the inputs and do error checking
  char* token0 = p.commands[0].tokens[0];
  char* token1 = p.commands[0].tokens[1];
  char* token2 = p.commands[0].tokens[2];

  int t0 = 0;
  if (token0 != NULL) {
    t0 = atoi(token0);
  }

  int t1 = 0;
  if (token1 != NULL) {
    t1 = atoi(token1);
  }

  // TODO: ENSURE THAT TOKEN0 AND TOKEN 1 ARE ONLY NUMERIC

  // Incorrect arguement count
  if(t0 < 1 || token2 != NULL){
    throw STSHException("Usage: " + builtin + " <jobid> <index> | <pid>.");
  } else {
    // If all of our inputs are correct
    if (token1 == NULL) { // IF there is just one arguement
      if(joblist.containsProcess(t0)){
        kill(t0, sig);
      } else {
        throw STSHException( "No process with pid " + to_string(t0) + ".");
      }
    } else { // IF there are two arguements
      if(!joblist.containsJob(t0)){
        throw STSHException("No job with id of " + to_string(t0) + ".");
      } else {
        STSHJob& job = joblist.getJob(t0);
        vector<STSHProcess>& processes = job.getProcesses();
	pid_t pid;
	if(t1 < 0 || t1 >= processes.size()){
          throw STSHException("Job " + to_string(t0) + " doesn't have a process at index " + to_string(t1) + ".");
        } else {
	  pid = processes[t1].getID();
          kill(pid, sig);
	}
      }
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
      fgbgHandler(pipeline, "fg", SIGCONT);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
    }
    break;
  case 3: // bg
    try {
      fgbgHandler(pipeline, "bg", SIGCONT);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
    }
    break;
  case 4: // slay 
    try {
      singleProcessHandler(pipeline, "slay", SIGKILL);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
    }
    break;
  case 5: // halt
    try {
      singleProcessHandler(pipeline, "halt", SIGSTOP);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
    }
    break; 
  case 6: // cont
    try {
      singleProcessHandler(pipeline, "cont", SIGCONT);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
    }
    break;
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
  while (true) {
    int status;
    pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED); 
    if (pid <= 0) break;
     
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
       updateJobList(joblist, pid, kTerminated);
       if (tcsetpgrp(STDIN_FILENO, getpid()) < 0) {
         throw STSHException("Failed to transfer STDIN control back to terminal.");
       }
    } else if(WIFSTOPPED(status)) {
       updateJobList(joblist, pid, kStopped);
       if (tcsetpgrp(STDIN_FILENO, getpid()) < 0) {
         throw STSHException("Failed to transfer STDIN control back to terminal.");
       } 
    } else { // WIFCONTINUED(status)
      updateJobList(joblist, pid, kRunning);
    }
  }
}

/* Function: sigForward
 * -------------------------------
 * Pass a signal to the foreground process
 */
static void sigForward(int sig){
  if(joblist.hasForegroundJob()){
    pid_t groupID = joblist.getForegroundJob().getGroupID();
    kill(-groupID, sig);
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
    setpgid(pid0, pid0);
    
    job.addProcess(STSHProcess(pid0, p.commands[0]));    
    
    // Print each of the group ids
    if(p.background) {
      cout << job.getGroupID();
      if( n == 1) cout << endl;
    } else {
    // If a process is running in the fg make sure it has keyboard control
      int error = tcsetpgrp(STDIN_FILENO, pid0);
      if (error < 0) {
        throw STSHException("Failed to transfer STDIN control to foreground process.");
      }
    }
  } else {
    setpgid(getpid(), getpid());
    
    if (!p.input.empty()) {
        int fdin = open(p.input.c_str(), O_RDONLY);
        dup2(fdin, STDIN_FILENO);
        close(fdin);
    }

    if (n==1) {
        if (!p.output.empty()) {
            int fdout;
            if (access(p.output.c_str(), F_OK) != -1 ) {
                fdout = open(p.output.c_str(), O_TRUNC | O_RDWR);
            } else {
                fdout = open(p.output.c_str(), O_CREAT | O_RDWR, 0644);
            }
            dup2(fdout, STDOUT_FILENO);
            close(fdout);
	} 
    } else {
        dup2(fds[1],STDOUT_FILENO);
        close(fds[0]);
    }
    
    char *combined[kMaxArguments + 1];
    combined[0] = (char *)p.commands[0].command;
    
    for (int i=1; i < (kMaxArguments + 1); i++) {
        combined[i] = (char *)p.commands[0].tokens[i-1];
    }

    execvp(combined[0], combined);
    
    if (n > 1){
      close(fds[1]);
    }

    throw(STSHException(string(p.commands[0].command) + ": Command not found."));
    
    exit(0);
  }
  
  // Middle proccesses  
  for (int i=1; i < (n-1); i++) {
      pipe2(fds + (i*2), O_CLOEXEC);
      pid_t pid_i = fork();
      if (pid_i != 0){
          setpgid(pid_i, pid0);
          job.addProcess(STSHProcess(pid_i, p.commands[i]));    
          
	  // Print each of the group ids
          if(p.background) {
              cout << " " << to_string(pid_i);
          }
      } else {
          setpgid(getpid(), pid0);
          dup2(fds[(2*i) + 1], STDOUT_FILENO);
          dup2(fds[2*(i-1)], STDIN_FILENO);

          char *combined[kMaxArguments + 1];
          combined[0] = (char *)p.commands[i].command;
    
          for (int j=1; j < (kMaxArguments + 1); j++) {
              combined[j] = (char *)p.commands[i].tokens[j-1];
          }

          execvp(combined[0], combined);
	  throw(STSHException(string(p.commands[i].command) + ": Command not found."));
          exit(0);
      }
  }

  // Last Process
  if (n > 1){ 
  pid_t pidl = fork();

  if (pidl != 0){
    setpgid(pidl, pid0);
    job.addProcess(STSHProcess(pidl, p.commands[n-1]));
    
    // Print each of the group ids
    if(p.background) {
        cout << " " << to_string(pidl) << endl;
    }
  } else {
    setpgid(getpid(), pid0);

    // Read from the output of the previous child/children
    dup2(fds[(2*n)-4],STDIN_FILENO);
    // Write to a file if a path is specified
    if (!p.output.empty()) {
      int fdout;
      if (access(p.output.c_str(), F_OK) != -1 ) {
          fdout = open(p.output.c_str(), O_TRUNC | O_RDWR);
      } else {
          fdout = open(p.output.c_str(), O_CREAT | O_RDWR, 0644);
      }
      dup2(fdout, STDOUT_FILENO);
      close(fdout);
    }

    char *combined[kMaxArguments + 1];
    combined[0] = (char *)p.commands[n-1].command;

    for (int i=1; i < (kMaxArguments + 1); i++) {
        combined[i] = (char *)p.commands[n-1].tokens[i-1];
    }

    close(fds[(n-2)*2+1]);
    execvp(combined[0], combined);
    close(fds[(n-2)*2]);
    throw(STSHException(string(p.commands[n-1].command) + ": Command not found.")); 
    exit(0);
  }
  }
  
  // Close all fds in the parent
  for (int i = 0; i < (n-1)*2 ; i++){
    close(fds[i]);
  } 

  // Run fg proccess in fg
  if(!p.background) {
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
