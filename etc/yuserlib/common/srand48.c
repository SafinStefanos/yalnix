/*
 * srand48.c
 */

#include <stdlib.h>
#include <stdint.h>


unsigned short __rand48_seed[3];	/* Common with lrand48.c, srand48.c */

void srand(unsigned int seed) {
  
  srand48( (long) seed );

}


void srand48(long seedval)
{
	__rand48_seed[0] = 0x330e;
	__rand48_seed[1] = (unsigned short)seedval;
	__rand48_seed[2] = (unsigned short)((uint32_t) seedval >> 16);
}




int rand(void) {

  int rc;

  rc = (int) jrand48(__rand48_seed);

  if  (rc < 0) rc = -rc;

  return rc;

}

long mrand48(void)
{
	return jrand48(__rand48_seed);
}
