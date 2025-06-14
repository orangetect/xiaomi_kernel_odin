#ifndef __KSU_H_APK_V2_SIGN
#define __KSU_H_APK_V2_SIGN

#include <linux/types.h>

bool ksu_is_manager_apk(char *path, char *package);

bool ksu_is_package_whitelisted(char *package);

#endif
