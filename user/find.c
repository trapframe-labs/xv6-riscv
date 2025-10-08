/*
  Write a simple version of the UNIX find program: find all the files in a directory tree with a specific name. Your solution should be in the file user/find.c.

  Some hints:
  * Look at user/ls.c to see how to read directories.
  * Use recursion to allow find to descend into sub-directories.
  * Don't recurse into "." and "..".
  * Changes to the file system persist across runs of qemu; to get a clean file system run make clean and then make qemu.
  * You'll need to use C strings. Have a look at K&R (the C book), for example Section 5.5.
  * Note that == does not compare strings like in Python. Use strcmp() instead.
  * Add the program to UPROGS in Makefile.
*/

#include <kernel/types.h>
#include <kernel/stat.h>
#include <kernel/fcntl.h>
#include <kernel/fs.h>
#include <user/user.h>

#define MAX_XV6_PATH_LEN    256

void createFullFilePath(char* buf, const char* path, char* file) {
  char* p;
  strcpy(buf, path);
  p = buf+strlen(buf);
  if(*(p-1)!='/')
    *(p++) = '/';
  strcpy(p, file);
}

int find(const char* path, const char* file) {
  struct dirent de;
  struct stat st;
  char buf[MAX_XV6_PATH_LEN];
  int fd;

  fd = open(path, O_RDONLY);
  if(fd < 0) {
    fprintf(2, "find: cannot open path = %s\n", path);
    return -1;
  }
  while(read(fd, &de, sizeof(de)) == sizeof(de)) {
    if(de.inum==0) {
      continue;
    }
    createFullFilePath(buf, path, de.name);
    if(stat(buf, &st) < 0) {
      fprintf(2, "find: cannot stat path = %s\n", path);
      continue;
    }
    // Format print
    if(strcmp(de.name, file)==0) {
      printf("%s/%s\n", path, file);
    }
    // Recurssion avoided for . and ..
    if((strcmp(de.name, ".") != 0) && (strcmp(de.name, "..") != 0)) {
      if(st.type==T_DIR)
        find(buf, file);
    }
  }
  close(fd);
  return 0;
}

int main(int argc, char* argv[]) {
  int ret = 0;
  if(argc != 3) {
    fprintf(2, "find: error! usage: find directory file\n");
    exit(-1);
  }
  ret = find(argv[1], argv[2]);         // directory to search in; file to search

  exit(ret);
}