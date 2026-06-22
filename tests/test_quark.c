#include <assert.h>
#include <string.h>

#include "quark/quark.h"

int main(void) {
    assert(strcmp(quark_version(), "0.1.0") == 0);
    return 0;
}
