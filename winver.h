// Support Win2K and up (https://learn.microsoft.com/en-us/cpp/porting/modifying-winver-and-win32-winnt)
#include <winsdkver.h>
#define _WIN32_WINNT 0x0500
#define WINVER       0x0500
#include <sdkddkver.h>
