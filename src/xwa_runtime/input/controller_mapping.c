#include "xwa_runtime/input/controller_mapping.h"
#include "xwa_runtime/input/hyperdrive_axis.h"

#include "aeron/aeron.h"

#include <string.h>

enum {
	CONTROLLER_AXIS_RANGE = 65535,
	CONTROLLER_AXIS_CENTER = 32768,
};

typedef struct XwaControllerMappingState {
	XwaControllerOptions options;
	int configured;
	XwaControllerOptions secondary;
	int secondary_configured;
	uint32_t previous_instance_id;
	uint32_t previous_secondary_id;
	uint64_t digital_axis_buttons[2];
	uint64_t pending_buttons[2];
	int previous_pov[2];
	uint32_t actions_instance_id[2];
	uint32_t hyperdrive_instance_id[2];
	int hyperdrive_armed[2];
} XwaControllerMappingState;

static XwaControllerMappingState g_controllerMapping;

static int ControllerMapping_ProfileEqual(const XwaControllerProfile* lhs, const XwaControllerProfile* rhs) {
	int i;

	if (lhs->pov_source != rhs->pov_source) {
		return 0;
	}
	for (i = 0; i < XWA_CONTROLLER_LOGICAL_AXIS_COUNT; ++i) {
		if (lhs->axes[i].source != rhs->axes[i].source || lhs->axes[i].invert != rhs->axes[i].invert ||
			lhs->axes[i].deadzone != rhs->axes[i].deadzone) {
			return 0;
		}
	}
	for (i = 0; i < XWA_CONTROLLER_LOGICAL_BUTTON_COUNT; ++i) {
		if (lhs->buttons[i].kind != rhs->buttons[i].kind || lhs->buttons[i].index != rhs->buttons[i].index ||
			lhs->buttons[i].threshold != rhs->buttons[i].threshold) {
			return 0;
		}
	}
	return memcmp(lhs->actions, rhs->actions, sizeof(lhs->actions)) == 0;
}

static int ControllerMapping_OptionsEqual(const XwaControllerOptions* lhs, const XwaControllerOptions* rhs) {
	return lhs->enabled == rhs->enabled && strcmp(lhs->device.guid, rhs->device.guid) == 0 &&
		   strcmp(lhs->device.path, rhs->device.path) == 0 && lhs->device.ordinal == rhs->device.ordinal &&
		   lhs->rumble_enabled == rhs->rumble_enabled &&
		   ControllerMapping_ProfileEqual(&lhs->gamepad, &rhs->gamepad) &&
		   ControllerMapping_ProfileEqual(&lhs->joystick, &rhs->joystick);
}

const AeronControllerSnapshot* XwaControllerMapping_ControllerForSlot(int slot) {
	const AeronInputSnapshot* input = Aeron_InputSnapshot();
	const XwaControllerOptions* options;
	const AeronControllerSnapshot* controller;
	if (!input || slot < 0 || slot > 1) return NULL;
	if (slot == 0) {
		if (!g_controllerMapping.configured) return NULL;
		options = &g_controllerMapping.options;
	} else {
		if (!g_controllerMapping.secondary_configured) return NULL;
		options = &g_controllerMapping.secondary;
	}
	if (!options->enabled) return NULL;
	controller = Aeron_SelectController(input, &options->device);
	if (slot == 1 && controller && controller == XwaControllerMapping_ControllerForSlot(0))
		return NULL; /* Never apply the same device twice. */
	return controller;
}

const AeronControllerSnapshot* XwaControllerMapping_SelectedController(void) {
	const AeronControllerSnapshot* first = XwaControllerMapping_ControllerForSlot(0);
	return first ? first : XwaControllerMapping_ControllerForSlot(1);
}

static const XwaControllerProfile* ControllerMapping_Profile(const XwaControllerOptions* options,
															 const AeronControllerSnapshot* controller) {
	return controller->kind == AERON_CONTROLLER_KIND_GAMEPAD ? &options->gamepad : &options->joystick;
}

static int16_t ControllerMapping_AxisValue(const AeronControllerSnapshot* controller, int source) {
	if (source < 0) {
		return 0;
	}
	if (controller->kind == AERON_CONTROLLER_KIND_GAMEPAD) {
		return source < AERON_GAMEPAD_AXIS_COUNT ? controller->gamepad_axes[source] : 0;
	}
	return source < controller->axis_count && source < AERON_CONTROLLER_AXIS_MAX
			   ? controller->raw_axes[source]
			   : 0;
}

static int ControllerMapping_IsTrigger(const AeronControllerSnapshot* controller, int source) {
	return controller->kind == AERON_CONTROLLER_KIND_GAMEPAD &&
		   (source == AERON_GAMEPAD_AXIS_LEFT_TRIGGER || source == AERON_GAMEPAD_AXIS_RIGHT_TRIGGER);
}

static uint32_t ControllerMapping_CenteredAxis(int16_t value, int invert, float deadzone) {
	double normalized = value < 0 ? (double)value / 32768.0 : (double)value / 32767.0;
	double magnitude;
	uint32_t mapped;

	magnitude = normalized < 0.0 ? -normalized : normalized;
	if (magnitude <= deadzone) {
		return CONTROLLER_AXIS_CENTER;
	}
	/* Shift SDL's complete signed range onto the complete WinMM range without
	 * losing the negative endpoint. */
	mapped = (uint32_t)((int32_t)value + CONTROLLER_AXIS_CENTER);
	return invert ? CONTROLLER_AXIS_RANGE - mapped : mapped;
}

static uint32_t ControllerMapping_TriggerAxis(int16_t value, int invert, float deadzone) {
	double normalized = (double)value / 32767.0;

	if (normalized < 0.0) {
		normalized = 0.0;
	} else if (normalized > 1.0) {
		normalized = 1.0;
	}
	if (normalized <= deadzone) {
		normalized = 0.0;
	}
	if (invert) {
		normalized = 1.0 - normalized;
	}
	return (uint32_t)(normalized * CONTROLLER_AXIS_RANGE);
}

static uint8_t ControllerMapping_Hat(const AeronControllerSnapshot* controller,
									 const XwaControllerProfile* profile) {
	uint8_t hat = AERON_CONTROLLER_HAT_CENTERED;

	if (controller->kind == AERON_CONTROLLER_KIND_GAMEPAD) {
		if (!profile->pov_source) {
			return hat;
		}
		if (controller->gamepad_buttons & (1u << AERON_GAMEPAD_BUTTON_DPAD_UP)) {
			hat |= AERON_CONTROLLER_HAT_UP;
		}
		if (controller->gamepad_buttons & (1u << AERON_GAMEPAD_BUTTON_DPAD_RIGHT)) {
			hat |= AERON_CONTROLLER_HAT_RIGHT;
		}
		if (controller->gamepad_buttons & (1u << AERON_GAMEPAD_BUTTON_DPAD_DOWN)) {
			hat |= AERON_CONTROLLER_HAT_DOWN;
		}
		if (controller->gamepad_buttons & (1u << AERON_GAMEPAD_BUTTON_DPAD_LEFT)) {
			hat |= AERON_CONTROLLER_HAT_LEFT;
		}
		return hat;
	}
	if (profile->pov_source >= 0 && profile->pov_source < controller->hat_count &&
		profile->pov_source < AERON_CONTROLLER_HAT_MAX) {
		return controller->raw_hats[profile->pov_source];
	}
	return hat;
}

static int ControllerMapping_PovDirection(uint8_t hat) {
	/* XWA has four POV actions. Vertical wins for diagonal hats. */
	if (hat & AERON_CONTROLLER_HAT_UP) {
		return 0;
	}
	if (hat & AERON_CONTROLLER_HAT_DOWN) {
		return 2;
	}
	if (hat & AERON_CONTROLLER_HAT_RIGHT) {
		return 1;
	}
	if (hat & AERON_CONTROLLER_HAT_LEFT) {
		return 3;
	}
	return -1;
}

static int ControllerMapping_HasPov(const AeronControllerSnapshot* controller,
									const XwaControllerProfile* profile) {
	if (controller->kind == AERON_CONTROLLER_KIND_GAMEPAD) {
		return profile->pov_source != 0;
	}
	return profile->pov_source >= 0 && profile->pov_source < controller->hat_count;
}

static void ControllerMapping_LogUnavailableSources(const AeronControllerSnapshot* controller,
											 const XwaControllerOptions* options) {
	const XwaControllerProfile* profile;
	int unavailable_axes = 0;
	int unavailable_buttons = 0;
	int unavailable_pov = 0;
	int i;

	if (!controller || controller->kind != AERON_CONTROLLER_KIND_JOYSTICK) {
		return;
	}
	profile = &options->joystick;
	for (i = 0; i < XWA_CONTROLLER_LOGICAL_AXIS_COUNT; ++i) {
		unavailable_axes += profile->axes[i].source >= controller->axis_count;
	}
	for (i = 0; i < XWA_CONTROLLER_LOGICAL_BUTTON_COUNT; ++i) {
		const AeronControllerDigitalSource* binding = &profile->buttons[i];
		if (binding->kind == AERON_CONTROLLER_DIGITAL_BUTTON) {
			unavailable_buttons += binding->index >= controller->button_count;
		} else if (binding->kind == AERON_CONTROLLER_DIGITAL_AXIS_POSITIVE ||
				   binding->kind == AERON_CONTROLLER_DIGITAL_AXIS_NEGATIVE) {
			unavailable_axes += binding->index >= controller->axis_count;
		}
	}
	unavailable_pov = profile->pov_source >= controller->hat_count;
	if (unavailable_axes || unavailable_buttons || unavailable_pov) {
		Aeron_LogWarn("xwa.input",
					  "Controller '%s' mapping references unavailable controls (%d axes, %d buttons, "
					  "invalid POV=%s)",
					  controller->name, unavailable_axes, unavailable_buttons,
					  unavailable_pov ? "yes" : "no");
	}
}

static void ControllerMapping_MapSnapshot(const XwaControllerOptions* options,
										  const AeronControllerSnapshot* controller, int has_focus,
										  uint64_t previous_axis_buttons, uint64_t* axis_buttons,
										  XwaControllerLogicalState* state) {
	const XwaControllerProfile* profile;
	int logical;

	if (axis_buttons) {
		*axis_buttons = 0;
	}
	if (!state) {
		return;
	}
	memset(state, 0, sizeof(*state));
	state->pov_direction = -1;
	for (logical = 0; logical < XWA_CONTROLLER_LOGICAL_AXIS_COUNT; ++logical) {
		state->axes[logical] = CONTROLLER_AXIS_CENTER;
		state->source_axes[logical] = -1;
	}
	if (!options || !controller || !controller->connected) {
		return;
	}
	profile = ControllerMapping_Profile(options, controller);
	state->has_pov = ControllerMapping_HasPov(controller, profile);
	for (logical = 0; logical < XWA_CONTROLLER_LOGICAL_AXIS_COUNT; ++logical) {
		/* Per-device Roll Enabled must not block another device's roll axis. */
		if (logical == XWA_CONTROLLER_AXIS_ROLL && !options->roll_enabled) continue;
		const XwaControllerAxisBinding* binding = &profile->axes[logical];
		const int16_t value = ControllerMapping_AxisValue(controller, binding->source);
		state->source_axes[logical] = (int8_t)binding->source;
		state->source_axis_values[logical] = value;
		if (!has_focus) {
			continue;
		}
		state->axes[logical] =
			ControllerMapping_IsTrigger(controller, binding->source)
				? ControllerMapping_TriggerAxis(value, binding->invert, binding->deadzone)
				: ControllerMapping_CenteredAxis(value, binding->invert, binding->deadzone);
	}
	if (!has_focus) {
		return;
	}
	for (logical = 0; logical < XWA_CONTROLLER_LOGICAL_BUTTON_COUNT; ++logical) {
		const AeronControllerDigitalSource* binding = &profile->buttons[logical];
		const uint64_t bit = UINT64_C(1) << logical;
		if (Aeron_ControllerDigitalSourceDown(controller, binding, (previous_axis_buttons & bit) != 0)) {
			state->buttons |= UINT64_C(1) << logical;
			if (axis_buttons && binding->kind != AERON_CONTROLLER_DIGITAL_BUTTON) {
				*axis_buttons |= bit;
			}
		}
	}
	state->pov_direction = ControllerMapping_PovDirection(ControllerMapping_Hat(controller, profile));
}

void XwaControllerMapping_MapSnapshot(const XwaControllerOptions* options,
									  const AeronControllerSnapshot* controller, int has_focus,
									  XwaControllerLogicalState* state) {
	ControllerMapping_MapSnapshot(options, controller, has_focus, 0, NULL, state);
}

void XwaControllerMapping_SetOptions(const XwaControllerOptions* options) {
	if (options && g_controllerMapping.configured &&
		ControllerMapping_OptionsEqual(options, &g_controllerMapping.options))
		return;
	if (g_controllerMapping.previous_instance_id)
		Aeron_RumbleController(g_controllerMapping.previous_instance_id, 0, 0, 0);
	if (options) g_controllerMapping.options = *options;
	else memset(&g_controllerMapping.options, 0, sizeof g_controllerMapping.options);
	g_controllerMapping.configured = options != NULL;
	g_controllerMapping.digital_axis_buttons[0] = 0;
	g_controllerMapping.pending_buttons[0] = 0;
	g_controllerMapping.previous_pov[0] = -1;
	g_controllerMapping.actions_instance_id[0] = 0;
	g_controllerMapping.hyperdrive_instance_id[0] = 0;
	g_controllerMapping.hyperdrive_armed[0] = 0;
}

void XwaControllerMapping_SetSecondaryOptions(const XwaControllerOptions* options) {
	if (options && g_controllerMapping.secondary_configured &&
		ControllerMapping_OptionsEqual(options, &g_controllerMapping.secondary))
		return;
	if (g_controllerMapping.previous_secondary_id)
		Aeron_RumbleController(g_controllerMapping.previous_secondary_id, 0, 0, 0);
	if (options) g_controllerMapping.secondary = *options;
	else memset(&g_controllerMapping.secondary, 0, sizeof g_controllerMapping.secondary);
	g_controllerMapping.secondary_configured = options != NULL;
	g_controllerMapping.digital_axis_buttons[1] = 0;
	g_controllerMapping.pending_buttons[1] = 0;
	g_controllerMapping.previous_pov[1] = -1;
	g_controllerMapping.actions_instance_id[1] = 0;
	g_controllerMapping.hyperdrive_instance_id[1] = 0;
	g_controllerMapping.hyperdrive_armed[1] = 0;
}

uint32_t XwaControllerMapping_SelectedInstanceId(void) {
	const AeronControllerSnapshot* controller = XwaControllerMapping_SelectedController();
	return controller ? controller->instance_id : 0;
}

int XwaControllerMapping_SelectedHasRumble(void) {
	const AeronControllerSnapshot* controller = XwaControllerMapping_SelectedController();
	return controller && controller->has_rumble;
}

int XwaControllerMapping_RumbleSlot(int slot, uint16_t low, uint16_t high, uint32_t duration_ms) {
	const AeronControllerSnapshot* controller = XwaControllerMapping_ControllerForSlot(slot);
	const XwaControllerOptions* opts = slot == 1 ? &g_controllerMapping.secondary : &g_controllerMapping.options;
	if (!controller || !controller->has_rumble) return 0;
	if (!opts->rumble_enabled && (low || high)) return 0;
	return Aeron_RumbleController(controller->instance_id, low, high, duration_ms);
}

int XwaControllerMapping_Rumble(uint16_t low, uint16_t high, uint32_t duration_ms) {
	return XwaControllerMapping_RumbleSlot(XwaControllerMapping_ControllerForSlot(0) ? 0 : 1,
										 low, high, duration_ms);
}

int XwaControllerMapping_ConsumeSelectionChange(void) {
	int changed = 0;
	for (int slot = 0; slot < 2; ++slot) {
		const AeronControllerSnapshot* controller = XwaControllerMapping_ControllerForSlot(slot);
		uint32_t id = controller ? controller->instance_id : 0;
		uint32_t* previous = slot ? &g_controllerMapping.previous_secondary_id
									 : &g_controllerMapping.previous_instance_id;
		if (id == *previous) continue;
		if (*previous) Aeron_RumbleController(*previous, 0, 0, 0);
		Aeron_LogInfo("xwa.input", "Controller %d changed from %u to %u", slot + 1, *previous, id);
		*previous = id;
		g_controllerMapping.digital_axis_buttons[slot] = 0;
		g_controllerMapping.pending_buttons[slot] = 0;
		g_controllerMapping.previous_pov[slot] = -1;
		g_controllerMapping.hyperdrive_instance_id[slot] = 0;
		g_controllerMapping.hyperdrive_armed[slot] = 0;
		if (controller) ControllerMapping_LogUnavailableSources(controller,
										 slot ? &g_controllerMapping.secondary : &g_controllerMapping.options);
		changed = 1;
	}
	return changed;
}

int XwaControllerMapping_GetState(XwaControllerLogicalState* state) {
	const AeronInputSnapshot* input = Aeron_InputSnapshot();
	int found = 0;
	if (!state) return 0;
	memset(state, 0, sizeof *state);
	state->pov_direction = -1;
	for (int i = 0; i < XWA_CONTROLLER_LOGICAL_AXIS_COUNT; ++i) {
		state->axes[i] = CONTROLLER_AXIS_CENTER;
		state->source_axes[i] = -1;
	}
	for (int slot = 0; slot < 2; ++slot) {
		const AeronControllerSnapshot* controller = XwaControllerMapping_ControllerForSlot(slot);
		const XwaControllerOptions* options = slot ? &g_controllerMapping.secondary : &g_controllerMapping.options;
		XwaControllerLogicalState current;
		if (!controller) continue;
		found = 1;
		ControllerMapping_MapSnapshot(options, controller, input && input->has_focus,
									 g_controllerMapping.digital_axis_buttons[slot],
									 &g_controllerMapping.digital_axis_buttons[slot], &current);
		for (int axis = 0; axis < XWA_CONTROLLER_LOGICAL_AXIS_COUNT; ++axis) {
			if (state->source_axes[axis] == -1 && current.source_axes[axis] >= 0) {
				state->axes[axis] = current.axes[axis];
				state->source_axes[axis] = current.source_axes[axis];
				state->source_axis_values[axis] = current.source_axis_values[axis];
			}
		}
		state->buttons |= current.buttons;
		if (!state->has_pov && current.has_pov) {
			state->has_pov = 1;
			state->pov_direction = current.pov_direction;
		}
	}
	return found;
}

int XwaControllerMapping_ConsumeHyperdriveEngage(void) {
	const AeronInputSnapshot* input = Aeron_InputSnapshot();
	int engage = 0;

	/* Neither controller may retain an armed lever across focus loss. */
	if (!input || !input->has_focus) {
		for (int slot = 0; slot < 2; ++slot) {
			g_controllerMapping.hyperdrive_instance_id[slot] = 0;
			g_controllerMapping.hyperdrive_armed[slot] = 0;
		}
		return 0;
	}
	for (int slot = 0; slot < 2; ++slot) {
		const AeronControllerSnapshot* controller = XwaControllerMapping_ControllerForSlot(slot);
		const XwaControllerOptions* options = slot ? &g_controllerMapping.secondary : &g_controllerMapping.options;
		const XwaControllerProfile* profile;
		const XwaControllerAxisBinding* binding;
		uint32_t value;
		int source;

		if (!controller || !controller->connected) {
			g_controllerMapping.hyperdrive_instance_id[slot] = 0;
			g_controllerMapping.hyperdrive_armed[slot] = 0;
			continue;
		}
		if (controller->instance_id != g_controllerMapping.hyperdrive_instance_id[slot]) {
			g_controllerMapping.hyperdrive_instance_id[slot] = controller->instance_id;
			g_controllerMapping.hyperdrive_armed[slot] = 0;
		}
		profile = ControllerMapping_Profile(options, controller);
		binding = &profile->axes[XWA_CONTROLLER_AXIS_HYPERDRIVE];
		source = binding->source;
		if (source < 0 || source >= (controller->kind == AERON_CONTROLLER_KIND_GAMEPAD
								   ? AERON_GAMEPAD_AXIS_COUNT : controller->axis_count)) {
			g_controllerMapping.hyperdrive_armed[slot] = 0;
			continue;
		}
		value = ControllerMapping_IsTrigger(controller, source)
			? ControllerMapping_TriggerAxis(ControllerMapping_AxisValue(controller, source),
										   binding->invert, binding->deadzone)
			: ControllerMapping_CenteredAxis(ControllerMapping_AxisValue(controller, source),
											binding->invert, binding->deadzone);
		engage |= XwaHyperdriveAxis_Update(value, &g_controllerMapping.hyperdrive_armed[slot]);
	}
	return engage;
}

void XwaControllerMapping_CopySelectedActions(uint16_t actions[20]) {
	const AeronControllerSnapshot* controller = XwaControllerMapping_SelectedController();
	const XwaControllerOptions* opts = XwaControllerMapping_ControllerForSlot(0)
										 ? &g_controllerMapping.options : &g_controllerMapping.secondary;
	const XwaControllerProfile* profile =
		controller && controller->kind == AERON_CONTROLLER_KIND_JOYSTICK ? &opts->joystick : &opts->gamepad;
	if (!actions) return;
	for (int i = 0; i < 16; ++i) actions[i] = profile->actions[i];
	for (int i = 0; i < 4; ++i) actions[16+i] = profile->actions[XWA_CONTROLLER_LOGICAL_BUTTON_COUNT+i];
}

/* All modern controller actions (64 buttons plus POV on each physical device)
 * are translated independently of WinMM's 16-button compatibility mask.
 * A held switch contributes modifiers, but ordinary actions fire on a fresh
 * press only; a keyboard key already in flight retains priority. */
void XwaControllerMapping_ReadActions(uint16_t* key, int* key_mods) {
	const AeronInputSnapshot* input = Aeron_InputSnapshot();
	if (!key || !key_mods || !input || !input->has_focus) return;
	for (int slot = 0; slot < 2; ++slot) {
		const AeronControllerSnapshot* controller = XwaControllerMapping_ControllerForSlot(slot);
		const XwaControllerOptions* options = slot ? &g_controllerMapping.secondary : &g_controllerMapping.options;
		const XwaControllerProfile* profile;
		XwaControllerLogicalState state;
		uint64_t previous, accepted = 0;
		int direction;
		if (!controller) {
			g_controllerMapping.pending_buttons[slot] = 0;
			g_controllerMapping.previous_pov[slot] = -1;
			g_controllerMapping.actions_instance_id[slot] = 0;
			continue;
		}
		profile = ControllerMapping_Profile(options, controller);
		ControllerMapping_MapSnapshot(options, controller, 1, g_controllerMapping.digital_axis_buttons[slot],
									 NULL, &state);
		/* On connection, do not synthesize presses from latching controls
		 * that were already on before the game/input device became active. */
		if (g_controllerMapping.actions_instance_id[slot] != controller->instance_id) {
			g_controllerMapping.actions_instance_id[slot] = controller->instance_id;
			g_controllerMapping.pending_buttons[slot] = state.buttons;
			g_controllerMapping.previous_pov[slot] = state.pov_direction;
		}
		previous = g_controllerMapping.pending_buttons[slot];
		for (int i = 0; i < XWA_CONTROLLER_LOGICAL_BUTTON_COUNT; ++i) {
			const uint64_t bit = UINT64_C(1) << i;
			const uint16_t action = profile->actions[i];
			if (!(state.buttons & bit)) continue;
			if (action == 156) *key_mods |= 1;
			else if (action == 157) *key_mods |= 2;
			else if (action && (action == 180 || action == 182 || action == 184 || action == 186 ||
							!(previous & bit))) {
				if (!*key) {
					*key = action;
					accepted |= bit;
				}
			}
		}
		g_controllerMapping.pending_buttons[slot] = (state.buttons & previous) | accepted;
		/* All released bits are cleared; modifiers are level-sensitive. */
		direction = state.has_pov ? state.pov_direction : -1;
		if (direction >= 0) {
			const uint16_t action = profile->actions[XWA_CONTROLLER_LOGICAL_BUTTON_COUNT + direction];
			if (action == 156) *key_mods |= 1;
			else if (action == 157) *key_mods |= 2;
			else if (action && (direction != g_controllerMapping.previous_pov[slot] ||
							 action == 180 || action == 182 || action == 184 || action == 186)) {
				if (!*key) *key = action;
			}
		}
		g_controllerMapping.previous_pov[slot] = direction;
	}
}
