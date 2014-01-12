#include <stdio.h>

#include "valgrind/memhist.h"

void bar(int** p)
{
  *p = NULL;
}

int foo()
{
  static int* vec[10];
  int i;

  VALGRIND_TRACK_MEM_WRITE(vec, 10*sizeof(int*), sizeof(int*));
  //VALGRIND_TRACK_MEM_WRITE(vec+5, 5*sizeof(int), 1);

  for (i=0; i<10; i++)
    {
      vec[i] = &i;
    }

  bar(&vec[7]);

  //VALGRIND_UNTRACK_MEM_WRITE(vec, 5*sizeof(int));

  for (i=0; i<10; i++)
    {
      printf("vec[%d] = %d\n", i, *vec[i]); 
    }

  //*(int*)0 = 17;

  //VALGRIND_UNTRACK_MEM_WRITE(vec+5, 5*sizeof(int));

  return 0;
}


int main()
{
	foo();
}
