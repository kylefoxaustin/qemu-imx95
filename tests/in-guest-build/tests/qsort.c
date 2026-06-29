// EXPECT: SORTED=1 2 3 4 5
#include <stdio.h>
#include <stdlib.h>
static int cmp(const void *a, const void *b){ return *(const int *)a - *(const int *)b; }
int main(void){ int v[]={3,1,4,2,5}; qsort(v,5,sizeof(int),cmp);
  printf("SORTED=%d %d %d %d %d\n", v[0],v[1],v[2],v[3],v[4]); return 0; }
