int printf(const char*, ...);

struct point { int x; int y; };

int main() {
    struct point p = {10, 20};
    printf("p.x=%d p.y=%d\n", p.x, p.y);

    struct point p2 = {1, 2};
    printf("p2.x=%d p2.y=%d\n", p2.x, p2.y);

    return 0;
}