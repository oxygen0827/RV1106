#include "../common/sys_manager/sys_manager.h"

_Static_assert(
    AICHAT_ACCESS_TOKEN_MAX_LENGTH == 64,
    "AIChat access token limit must remain 64 characters"
);

_Static_assert(
    sizeof(((AIChatAppInfo_t *)0)->token) == AICHAT_ACCESS_TOKEN_MAX_LENGTH + 1,
    "AIChat token buffer must include space for the terminator"
);

int main(void) {
    return 0;
}
