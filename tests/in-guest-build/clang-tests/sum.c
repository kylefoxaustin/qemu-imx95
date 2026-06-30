// EXPECT: CLANG SUM1..100=5050
#include <stdio.h>
int main(void){ long s=0; for (int i=1;i<=100;i++) s+=i; printf("CLANG SUM1..100=%ld\n", s); return 0; }
