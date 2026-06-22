#include <stdio.h>

#include "quark/quark.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("quark %s\n", quark_version());

    return 0;
}
