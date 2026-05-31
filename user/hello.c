 // user/hello.c 

// Call kernel hello syscall. 

 

#include "kernel/types.h" 

#include "kernel/stat.h" 

#include "user.h" 

 

int 

main(void) 

{ 

  // call kernel-side hello 

  hello(); 

 

  // We can also print from user side if desired 

  printf("user: hello() returned, back in user mode\n"); 

  exit(0); 

}  