#include "options/head_tracking_page.h"

#include "options/options_window.h"
#include "xwa_runtime/config/modern_input_options.h"
#include "xwa_runtime/input/head_tracking.h"

/* The head tracking options apply at once and are saved on Close. */
void XwaHeadTrackingPage_Draw(AeronUiContext* ui, const AeronInputSnapshot* input) {
#ifdef _WIN32
	static const char* const sources[] = { "TrackIR", "OpenTrack UDP" };
#else
	/* Native TrackIR is Windows-only; the one choice is UDP. */
	static const char* const sources[] = { "OpenTrack UDP" };
#endif
	static const char* const invert_labels[XWA_HEAD_TRACK_AXIS_COUNT] = { "Invert Yaw",  "Invert Pitch",
																		  "Invert Roll", "Invert X",
																		  "Invert Y",    "Invert Z" };
	static XwaModernInputOptions options;
	(void)input;
	XwaModernInputOptions_Get(&options);
	const int was_enabled = options.head_tracking.enabled != 0;
#ifdef _WIN32
	int source = options.head_tracking.source;
#else
	int source = 0;
#endif

	AeronUi_Header(ui, "Head Tracking");
	bool changed = AeronUi_Toggle(ui, "Head Tracking", &options.head_tracking.enabled);
	if (AeronUi_SelectorEnabled(ui, "Head Tracking Source", &source, sources,
								(int)(sizeof sources / sizeof sources[0]), options.head_tracking.enabled)) {
		changed = true;
	}
#ifdef _WIN32
	options.head_tracking.source = source;
	if (options.head_tracking.source == XWA_HEAD_TRACK_SOURCE_TRACKIR)
		AeronUi_Help(ui, "Uses the installed TrackIR software. Start it before flying.");
	else
#endif
		AeronUi_Help(ui, "In OpenTrack, set the output to \"UDP over network\" with the address 127.0.0.1 "
						 "and port 4242.");
#ifndef _WIN32
	options.head_tracking.source = XWA_HEAD_TRACK_SOURCE_OPENTRACK_UDP;
#endif

	AeronUi_Header(ui, "Inversion");
	AeronUi_Help(ui, "Inversion is relative to OpenXWA's standard head-motion directions.");
	for (int axis = 0; axis < XWA_HEAD_TRACK_AXIS_COUNT; ++axis)
		changed |= AeronUi_Toggle(ui, invert_labels[axis], &options.head_tracking.invert[axis]);

	if (AeronUi_Button(ui, "Restore Head Tracking Defaults")) {
		static XwaModernInputOptions defaults;
		XwaModernInputOptions_GetDefaults(&defaults);
		options.head_tracking = defaults.head_tracking;
		changed = true;
	}
	if (!changed)
		return;
	if (!XwaModernInputOptions_Set(&options)) {
		XwaOptionsWindow_ReportError("The head tracking options are invalid.");
		return;
	}
	/* Disconnect at once, also outside a mission where no camera poll
	 * would notice the change. */
	if (was_enabled && !options.head_tracking.enabled)
		XwaHeadTracking_Shutdown();
}
