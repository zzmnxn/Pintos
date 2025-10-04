/* additional.c
   
   Test program for additional system calls fibonacci and max_of_four_int.
   Takes four integer arguments and outputs:
   - The fibonacci number of the first argument
   - The maximum of all four arguments
*/

#include <stdio.h>
#include <stdlib.h>
#include <syscall.h>

int
main (int argc, char *argv[])
{
  int a, b, c, d;
  int fib_result, max_result;
  
  /* Check if we have exactly 4 arguments */
  if (argc != 5)
    {
      printf ("Usage: additional [num1] [num2] [num3] [num4]\n");
      return EXIT_FAILURE;
    }
  
  /* Convert arguments to integers */
  a = atoi (argv[1]);
  b = atoi (argv[2]);
  c = atoi (argv[3]);
  d = atoi (argv[4]);
  
  /* Call fibonacci system call with first argument */
  fib_result = fibonacci (a);
  
  /* Call max_of_four_int system call with all four arguments */
  max_result = max_of_four_int (a, b, c, d);
  
  /* Print results separated by space */
  printf ("%d %d\n", fib_result, max_result);
  
  return EXIT_SUCCESS;
}

