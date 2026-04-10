#include <cstdio>
#include <cstddef>
#include "src/from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"

using P = from::paramdef::WORLD_MAP_POINT_PARAM_ST;

int main() {
    printf("sizeof(WORLD_MAP_POINT_PARAM_ST) = 0x%zX (%zu)\n", sizeof(P), sizeof(P));
    printf("offsetof eventFlagId     = 0x%02zX\n", offsetof(P, eventFlagId));
    printf("offsetof iconId          = 0x%02zX\n", offsetof(P, iconId));
    printf("offsetof areaNo          = 0x%02zX\n", offsetof(P, areaNo));
    printf("offsetof gridXNo         = 0x%02zX\n", offsetof(P, gridXNo));
    printf("offsetof gridZNo         = 0x%02zX\n", offsetof(P, gridZNo));
    printf("offsetof posX            = 0x%02zX\n", offsetof(P, posX));
    printf("offsetof posZ            = 0x%02zX\n", offsetof(P, posZ));
    return 0;
}
