void exit(int);

#define m(L) (L'1' + (L))
int main(void) {
    exit(m(0) != L'1');
}
/* { dg-require-effective-target nested } */
