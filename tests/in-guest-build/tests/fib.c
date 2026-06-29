// EXPECT: FIB10=55
#include <stdio.h>
int main(void){ int a=0,b=1; for (int i=0;i<10;i++){ int t=a+b; a=b; b=t; } printf("FIB10=%d\n", a); return 0; }
