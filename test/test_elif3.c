#if TEST == 1
int x = 1;
#elif TEST == 2
int x = 2;
#endif
int y = 10;

int main() { return x + y; }