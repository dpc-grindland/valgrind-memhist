#include <stdio.h>

#include "valgrind/memhist.h"

void bar(long* p, long value)
{
  *p = value;
}

int foo()
{
  static long vec[10];
  int i;

  VALGRIND_TRACK_MEM_WRITE(vec, sizeof(vec), sizeof(long), 3, "vec");
  //VALGRIND_TRACK_MEM_WRITE(vec+5, 5*sizeof(int), 1);

  for (i=0; i<8; i++)
    {
      vec[i] = i*0x1000 + i + 1;
    }

  //bar(&vec[7], NULL);

  bar(&vec[3], __LINE__);
  bar(&vec[3], __LINE__);

  bar(&vec[4], __LINE__);
  bar(&vec[4], __LINE__);
  bar(&vec[4], __LINE__);

  //VALGRIND_UNTRACK_MEM_WRITE(vec, 5*sizeof(int));

  for (i=0; i<10; i++)
    {
      printf("vec[%d] = %lx\n", i, vec[i]);
    }

  //*(int*)0 = 17;

  //VALGRIND_UNTRACK_MEM_WRITE(vec+5, 5*sizeof(int));

  return 0;
}


int main()
{
	foo();
}
