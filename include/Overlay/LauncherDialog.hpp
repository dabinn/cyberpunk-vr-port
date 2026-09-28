#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void ShowLauncherDialog();
int IsLauncherOpen();
void WaitForLauncherStartup(unsigned int milliseconds);

#ifdef __cplusplus
}
#endif
