/*
 * Streets of Rage for the PlayStation 5 - what runs before main, and the player's notification.
 *
 * sor's main is the same as on the desktop, so the console's start-up is a constructor:
 * standard error into klog and the shell's splash dismissed.
 *
 * After griches' PS5_DevilutionX.
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "platform.h"

int sceKernelSendNotificationRequest(uint32_t device, void *request, size_t size, int blocking);

__attribute__((constructor)) static void
ps5_startup(void)
{
   platform_init("Streets of Rage");
}

/* A line in the corner of the screen, for what the player must do when there is no game to show it in. */
void
ps5_notify(const char *message)
{
   struct {
      int type, request_id, priority, message_id, target_id, user_id, unknown1, unknown2, app_id, error, unknown3;
      unsigned char use_icon;
      char message[1024], icon[1024], unknown[1024];
   } request;

   memset(&request, 0, sizeof request);
   request.target_id = -1;
   strncpy(request.message, message, sizeof request.message - 1);
   say("%s", message);
   (void)sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}
