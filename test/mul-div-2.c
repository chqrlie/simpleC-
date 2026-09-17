#include <limits.h>
#include <stdio.h>

long n = 1;
long d = -1;

int main() {
    printf("n = %ld, d = %ld\n", n, d);
    printf("1 / -1 = %d\n",  1 / -1);
    printf("1 /  d = %ld\n", 1 / d);
    printf("n / -1 = %ld\n", n / -1);
    printf("n /  d = %ld\n", n / d);
    printf("1 %% -1 = %d\n",  1 % -1);
    printf("1 %%  d = %ld\n", 1 % d);
    printf("n %% -1 = %ld\n", n % -1);
    printf("n %%  d = %ld\n", n % d);

    n = d = LONG_MIN;

    printf("n = %ld, d = %ld\n", n, d);
    printf("LONG_MIN / LONG_MIN = %ld\n", LONG_MIN / LONG_MIN);
    printf("LONG_MIN / d        = %ld\n", LONG_MIN / d);
    printf("n        / LONG_MIN = %ld\n", n / LONG_MIN);
    printf("n        /  d       = %ld\n", n / d);

    printf("LONG_MIN %% LONG_MIN = %ld\n", LONG_MIN % LONG_MIN);
    printf("LONG_MIN %% d        = %ld\n", LONG_MIN % d);
    printf("n        %% LONG_MIN = %ld\n", n % LONG_MIN);
    printf("n        %% d        = %ld\n", n % d);

    n = d = LONG_MAX;

    printf("n = %ld, d = %ld\n", n, d);
    printf("LONG_MAX / LONG_MAX = %ld\n", LONG_MAX / LONG_MAX);
    printf("LONG_MAX / d        = %ld\n", LONG_MAX / d);
    printf("n        / LONG_MAX = %ld\n", n / LONG_MAX);
    printf("n        /  d       = %ld\n", n / d);

    printf("LONG_MAX %% LONG_MAX = %ld\n", LONG_MAX % LONG_MAX);
    printf("LONG_MAX %% d        = %ld\n", LONG_MAX % d);
    printf("n        %% LONG_MAX = %ld\n", n % LONG_MAX);
    printf("n        %% d        = %ld\n", n % d);

    return 0;
}
