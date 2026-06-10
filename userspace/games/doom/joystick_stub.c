/* userspace/games/doom/joystick_stub.c — no-op joystick backend.
 *
 * DOOM's input layer expects these entry points to exist. We have no
 * joystick hardware support, so each one is a do-nothing stub. Add a real
 * backend here when gamepad support lands.
 */
#include "doomgeneric/doomtype.h"

void I_InitJoystick(void)              { }
void I_ShutdownJoystick(void)          { }
void I_BindJoystickVariables(void)     { }
void I_UpdateJoystick(void)            { }
