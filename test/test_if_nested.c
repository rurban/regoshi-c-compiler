#if TEST == 1
    #if PACK
    int x = 2;
    #else
    int x = 1;
    #endif
#else
int x = 0;
#endif

int main() { return x; }