/*
  Write a program that uses UNIX system calls to ''ping-pong'' a byte between two processes over a pair of pipes, one for each direction. The parent should send a byte to the child; the child should print "<pid>: received ping", where <pid> is its process ID, write the byte on the pipe to the parent, and exit; the parent should read the byte from the child, print "<pid>: received pong", and exit. Your solution should be in the file user/pingpong.c.

  Some hints:
  * Use pipe to create a pipe.
  * Use fork to create a child.
  * Use read to read from the pipe, and write to write to the pipe.
  * Use getpid to find the process ID of the calling process.
  * Add the program to UPROGS in Makefile.
  * User programs on xv6 have a limited set of library functions available to them. You can see the list in user/user.h; the source (other than for system calls) is in user/ulib.c, user/printf.c, and user/umalloc.c.
*/

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define ONE_BYTE  1
#define MSG_PING 'p'
#define MSG_PONG 'c'
s
void rearrange_stdio(int main_fd, int pipe_fd) {
  close(main_fd);
  dup(pipe_fd);
}

void close_pipes(int* pipe1, int* pipe2) {
  close(pipe1[0]);
  close(pipe1[1]);
  close(pipe2[0]);
  close(pipe2[1]);
}

int main(int argc, char* argv) {
  char msg_ping = MSG_PING, msg_pong = MSG_PONG;
  char read_byte1='x', read_byte2='x';
  int p_stdout, c_stdout;
  int pipe1[2];                         // pipe from parent to child
  int pipe2[2];                         // pipe from child to parent
  int res=0;
  
  // create 2 set of pipes - read/write on both ends
  pipe(pipe1);
  pipe(pipe2);

  if(fork()==0){                         // child process
    c_stdout = dup(1);                                      // save this for printing ping later

    // read in child process
    rearrange_stdio(0, pipe1[0]);                           // child stdin -> pipe1[0]
    res = read(0, &read_byte1, sizeof(read_byte1));         // blocking read on new stdin
    if(res!=ONE_BYTE) {
      fprintf(c_stdout, "pingpong: child read failed\n");
      exit(1);
    }
    if(read_byte1 == MSG_PING) {
      fprintf(c_stdout, "%d: received ping\n", getpid()); // print to child's saved stdout
    }

    // write end of pipe2
    rearrange_stdio(1, pipe2[1]);                           // child stdout -> pipe2[1]
    res = write(1, &msg_pong, sizeof(msg_pong));            // write to new stdout
    if(res!=ONE_BYTE) {
      fprintf(c_stdout, "pingpong: child write failed\n");
      exit(1);
    }
    
    // cleanup
    close(c_stdout);
    close_pipes(pipe1, pipe2);                       /// close all pipes
    exit(0);

  } else {                               // parent process
    p_stdout = dup(1);                                       // save this for printing pong later
    rearrange_stdio(1, pipe1[1]);                            // parent stdout -> pipe1[1]
    rearrange_stdio(0, pipe2[0]);                            // parent stdin -> pipe2[0]
    
    // write in parent
    res = write(1, &msg_ping, sizeof(msg_ping));             // write to new stdout
    if(res!=ONE_BYTE) {
      fprintf(p_stdout, "pingpong: parent write failed\n");
      exit(1);
    }

    // read in parent
    res = read(0, &read_byte2, sizeof(read_byte2));          // read from new stdin
    if(res!=ONE_BYTE) {
      fprintf(p_stdout, "pingpong: parent read failed\n");
      exit(1);
    }
    if(read_byte2 == MSG_PONG) {
      fprintf(p_stdout, "%d: received pong\n", getpid());  // write to general stdout
    }
    
    close(p_stdout);
    close_pipes(pipe1, pipe2);
    exit(0);
  }

  exit(0);
}