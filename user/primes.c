/*
  Write a concurrent version of prime sieve using pipes. This idea is due to Doug McIlroy, inventor of Unix pipes.
  Breaking down into simpler tasks:

  Some hints:
  * Be careful to close file descriptors that a process doesn't need, because otherwise your program will run xv6 out of resources before the first process reaches 35.
  * Once the first process reaches 35, it should wait until the entire pipeline terminates, including all children, grandchildren, &c. Thus the main prime process should only exit after all the output has been printed, and after all the other prime processes have exited.
  * Hint: read returns zero when the write-side of a pipe is closed.
  * It's simplest to directly write 32-bit (4-byte) ints to the pipes, rather than using formatted ASCII I/O.
  * You should create the processes in the pipeline only as they are needed.
  * Add the program to UPROGS in Makefile.
*/

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int isPrime(int n) {
  if(n<2) return 0;
  if(n==2 || n==3)  return 1;
  if(n%2==0 || n%3==0)  return 0;
  for(int i=5; i*i<=n; i+=6) {
    if((n%(i)==0) || (n%(i+2)==0)) {
      return 0;
    }
  }
  return 1;
}

void createProcess(int* p, int num) {
  if((num<2) || (num>35)) {                // base case
    return;
  }
  int ret = fork();
  if(ret<0) {
    close(p[0]);
    close(p[1]);
    fprintf(1, "prime: parent fork failed!\n");
    exit(1);
  } else if(ret>0) {          // parent process
    close(0);                 // close stdin
    close(1);                 // close stdout
    close(p[0]);              // close input end of pipe in parent
    write(p[1], &num, sizeof(int));               // write to pipe's left end
    wait(0);
    close(p[1]);                                  // close pipe out after write is done
  } else {                    // child process
    close(p[1]);              // close pipe out
    int n;
    int ret = read(p[0], &n, sizeof(int));        // read from pipe's right end
    close(p[0]);              // close pipe in
    if(ret<0) {
      fprintf(1, "prime: child read error!\n");
      exit(1);
    }
    fprintf(1, "prime %d\n", n);
    
    // Prime logic
    while(isPrime(++n)==0);

    // create new pipes
    if(pipe(p) < 0) {
      fprintf(1, "prime: child pipe creation failed\n");
      exit(1);
    }
    createProcess(p, n);
  }
}

int main() {
  int p[2];
  // create a pipe
  if(pipe(p) < 0) {
    fprintf(1, "prime: pipe creation failed\n");
    exit(1);
  }
  createProcess(p, 2);
  exit(0);
}