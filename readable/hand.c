/* The switch between code rewritten by hand and the generated code it replaces (recomp/patches.py HAND). */
#include <stdlib.h>
#include "hand.h"

int rt_hand = 1;

__attribute__((constructor)) static void hand_switch(void) {
    const char *e = getenv("MHFU_HAND");
    if (e && e[0] == '0')
        rt_hand = 0;
}
