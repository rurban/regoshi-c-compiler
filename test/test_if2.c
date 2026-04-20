#define VAL 5

#if VAL > 3
int a = 1;
#elif VAL > 1
int a = 2;
#else
int a = 0;
#endif

#if 0
int should_be_removed = 999;
#endif

int present = 42;

int main() { return 0; }