#if 1 < 1
#error 1 < 1 is a bug
#elif 1 > 1
#error 1 > 1 is a bug
#else
#warning 1 == 1 is OK
int main(){}
#endif
