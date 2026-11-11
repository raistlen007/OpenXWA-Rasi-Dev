/* XWA controller source for the Aeron WinMM joystick shim. */

#include "xwa_runtime/input/winmm_joystick_provider.h"

#include "aeron/compat/host.h"
#include "xwa_runtime/input/controller_mapping.h"

#include <string.h>

static WinmmJoystickTraceSample g_winmmJoystickTraceSample;
static int g_winmmJoystickTraceValid;

static int XwaWinmmJoystick_Source(AeronWinmmJoystickState* out, void* user) {
	const AeronControllerSnapshot* controller;
	XwaControllerLogicalState state;
	WinmmJoystickTraceSample trace;

	(void)user;
	controller = XwaControllerMapping_SelectedController();
	if (!controller || !XwaControllerMapping_GetState(&state)) {
		return 0;
	}
	out->axes[0] = state.axes[XWA_CONTROLLER_AXIS_YAW];
	out->axes[1] = state.axes[XWA_CONTROLLER_AXIS_PITCH];
	out->axes[2] = state.axes[XWA_CONTROLLER_AXIS_THROTTLE];
	out->axes[3] = state.axes[XWA_CONTROLLER_AXIS_ROLL];
	/* Modern button actions are dispatched directly for all 64 slots per
	 * controller; exposing them again through WinMM would double-fire. */
	out->buttons = 0;
	out->button_count = XWA_CONTROLLER_LOGICAL_BUTTON_COUNT;
	out->pov_direction = -1;
	out->has_pov = 0;
	out->name = controller->name;

	memset(&trace, 0, sizeof(trace));
	trace.deviceId = controller->instance_id;
	trace.sourceAxisX = state.source_axes[XWA_CONTROLLER_AXIS_YAW];
	trace.sourceAxisY = state.source_axes[XWA_CONTROLLER_AXIS_PITCH];
	trace.sourceAxisR = state.source_axes[XWA_CONTROLLER_AXIS_ROLL];
	trace.sourceValueX = state.source_axis_values[XWA_CONTROLLER_AXIS_YAW];
	trace.sourceValueY = state.source_axis_values[XWA_CONTROLLER_AXIS_PITCH];
	trace.sourceValueR = state.source_axis_values[XWA_CONTROLLER_AXIS_ROLL];
	trace.winmmX = out->axes[0];
	trace.winmmY = out->axes[1];
	trace.winmmR = out->axes[3];
	g_winmmJoystickTraceSample = trace;
	g_winmmJoystickTraceValid = 1;
	return 1;
}

void XwaWinmmJoystick_RegisterSource(void) { AeronCompat_SetJoystickSource(XwaWinmmJoystick_Source, NULL); }

int WinmmJoystick_GetLastTraceSample(WinmmJoystickTraceSample* sample) {
	if (!sample || !g_winmmJoystickTraceValid) {
		return 0;
	}
	*sample = g_winmmJoystickTraceSample;
	return 1;
}

void WinmmJoystick_ResetTrace(void) {
	memset(&g_winmmJoystickTraceSample, 0, sizeof(g_winmmJoystickTraceSample));
	g_winmmJoystickTraceValid = 0;
}
