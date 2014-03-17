#include <stdio.h>
#include <stdlib.h>

#include "valgrind/memhist.h"

#define ASSERT(C) ((C) ? (void)0 : assert_error(#C, __FILE__,__LINE__))

static void assert_error(const char* expr, const char* file, int line)
{
    fprintf(stderr, "Assertion failed: %s in %s, line %d\n",
	    expr, file, line);
    fflush(stderr);
    abort();
}

static __inline__
int cas32(int *var, int wrt, int old)
{
    __asm__ __volatile__(
      "lock; cmpxchgl %2, %3"
      : "=a"(old), "=m"(*var)
      : "r"(wrt), "m"(*var), "0"(old)
      : "cc", "memory"); /* full memory clobber to make this a compiler barrier */
    return old;
}


void test_cas(void)
{
    int value = 17;
    int was;

    VALGRIND_TRACK_MEM_WRITE(&value, sizeof(value), sizeof(value), 3, "test_cas");

    was = cas32(&value, 18, 17);
    ASSERT(was == 17);
    ASSERT(value == 18);

    was = cas32(&value, 19, 17);
    ASSERT(was == 18);
    ASSERT(value == 18);

    VALGRIND_TRACK_DISABLE(&value, sizeof(value));

}


void bar(long* p, long value)
{
  *p = value;
}

int foo()
{
  static long vec[10];
  int i;

  test_cas();

  VALGRIND_TRACK_MEM_WRITE(vec, sizeof(vec), sizeof(long), 3, "vec");
  //VALGRIND_TRACK_MEM_WRITE(vec+5, 5*sizeof(int), 1);

  for (i=0; i<8; i++)
    {
      vec[i] = i*0x1000 + i + 1;
    }

  //bar(&vec[7], NULL);

  VALGRIND_SET_PROTECTION(vec, sizeof(vec), "vec", VG_MEM_NOWRITE);

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
