/*
  Write a simple version of the UNIX xargs program: read lines from the standard input and run a command for each line, supplying the line as arguments to the command. Your solution should be in the file user/xargs.c.
  
  Some hints:
  * Use fork and exec to invoke the command on each line of input. Use wait in the parent to wait for the child to complete the command.
  * To read individual lines of input, read a character at a time until a newline ('\n') appears.
  * kernel/param.h declares MAXARG, which may be useful if you need to declare an argv array.
  * Add the program to UPROGS in Makefile.
  * Changes to the file system persist across runs of qemu; to get a clean file system run make clean and then make qemu.
*/

#include <kernel/types.h>
#include <kernel/param.h>
#include <kernel/stat.h>
#include <user/user.h>

// MAXARG for exec in xv6 = 32. Each arg size = max of 10 characters. 512 is big enough
#define ARGSIZE   10

int main(int argc, char* argv[]) {
  int i, offset;
  int ret;
  char buf[ARGSIZE*MAXARG];
  char* args[MAXARG];

  i=0; offset=0;
  // chop argv[0]
  for(int i=1; i<argc; ++i) {
    args[i-1] = argv[i];
  }
  argc--;

  while(read(0, &buf[i], sizeof(char)) != 0) {          // reading through stdin until pipe closed
    if(buf[i]==' ' || buf[i]=='\n') {
      if(argc>MAXARG) {
        fprintf(2, "xargs: too many args\n");
        exit(1);
      }
      buf[i] = 0;           // nullterminate
      args[argc] = &buf[offset];
      argc++;
      offset += (i+1);      // new arg location
      i = offset;
    } else {
      ++i;
    }
  }

  ret = fork();
  
  if(ret>0) {               // parent process
    wait(0);
  } else if(ret==0) {
    // exec does not return on success. If exec returns, it's an error
    args[argc]=0;             // end point for exec
    exec(args[0], args);
    fprintf(2, "exec failed!");
    exit(1);
  } else {
    fprintf(2, "xargs: fork failed\n");
  }

  exit(0);
}