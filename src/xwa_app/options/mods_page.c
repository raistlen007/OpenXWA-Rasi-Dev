#include "options/mods_page.h"

#include "aeron/aeron.h"
#include "aeron/time.h"
#include "mod_format.h"
#include "options/mods_page_internal.h"
#include "options/options_window.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long the list waits after the last key in the mods folder field before
 * it lists the mods of the new folder. */
#define MODS_FOLDER_DELAY_US 700000u

XwaModsPage g_xwaModsPage;

/* ---- Configuration ------------------------------------------------------ */

void XwaModsPage_LoadConfig(void) {
	XwaModsPage* p = &g_xwaModsPage;
	char error[512];
	p->config_ok = XwaInstallPaths_Load(Aeron_GetVfs(), &p->config, error, sizeof error);
	if (p->config_ok)
		p->config_message[0] = '\0';
	else
		snprintf(p->config_message, sizeof p->config_message,
				 "OpenXWA can't read config.yaml, so you can't change the mod: %s", error);
	if (!XwaModList_Folder(p->config.mods, p->saved_mods_folder, sizeof p->saved_mods_folder))
		p->saved_mods_folder[0] = '\0';
}

void XwaModsPage_ApplyModsFolder(void) {
	XwaModsPage* p = &g_xwaModsPage;
	char error[512];
	p->mods_folder_edit_us = 0;
	p->mods_folder_message[0] = '\0';
	if (!p->mods_folder[0]) {
		snprintf(p->mods_folder_message, sizeof p->mods_folder_message,
				 "Choose the folder that has your mods.");
		return;
	}
	if (XwaModList_SamePath(p->mods_folder, p->saved_mods_folder))
		return;
	if (!XwaModList_IsFolder(p->mods_folder)) {
		snprintf(p->mods_folder_message, sizeof p->mods_folder_message, "This folder doesn't exist.");
		return;
	}
	if (!p->config_ok) {
		snprintf(p->mods_folder_message, sizeof p->mods_folder_message,
				 "OpenXWA can't read config.yaml, so you can't change the mods folder.");
		return;
	}
	if (!XwaInstallPaths_SaveMods(Aeron_GetVfs(), p->mods_folder, error, sizeof error)) {
		snprintf(p->mods_folder_message, sizeof p->mods_folder_message,
				 "OpenXWA can't save the mods folder: %s", error);
		return;
	}
	XwaModsPage_LoadConfig();
	p->mods_scanned = 0;
	p->mod_selected = SIZE_MAX;
	p->mods_message[0] = '\0';
	/* The form's mod folder follows. */
	p->checked = 0;
}

bool XwaModsPage_UseMod(const char* path, char* message, size_t capacity) {
	XwaModsPage* p = &g_xwaModsPage;
	char error[512];
	/* The original game keeps paths.mod, so the mod stays in the list. */
	const int saved = path[0]
						  ? XwaInstallPaths_SaveMod(Aeron_GetVfs(), path, 1, error, sizeof error)
						  : XwaInstallPaths_SaveMod(Aeron_GetVfs(), p->config.mod, 0, error, sizeof error);
	if (saved) {
		message[0] = '\0';
		XwaModsPage_LoadConfig();
	} else {
		snprintf(message, capacity, "OpenXWA can't save the mod choice: %s", error);
	}
	p->mods_scanned = 0;
	return saved != 0;
}

float XwaModsPage_ContentHeight(void) { return XwaOptionsWindow_ScrollHeight(); }

/* ---- The folder dialog -------------------------------------------------- */

static char* browse_field(XwaModsBrowse browse) {
	XwaModsPage* p = &g_xwaModsPage;
	switch (browse) {
		case XWA_MODS_BROWSE_SOURCE:
			return p->source;
		case XWA_MODS_BROWSE_AUTHORING:
			return p->authoring;
		case XWA_MODS_BROWSE_MODS_LIST:
			break;
	}
	return p->mods_folder;
}

static void open_dialog(XwaModsBrowse browse, const char* title) {
	XwaModsPage* p = &g_xwaModsPage;
	if (p->dialog)
		return;
	const AeronFolderDialogOptions options = {
		.title = title,
		.accept_label = "Choose",
		.cancel_label = "Cancel",
		.initial_path = browse_field(browse)[0] ? browse_field(browse) : NULL,
	};
	p->dialog = Aeron_ShowOpenFolderDialog(&options);
	p->browse = browse;
}

static void poll_dialog(void) {
	XwaModsPage* p = &g_xwaModsPage;
	char path[XWA_MODS_PATH_CAP], error[256];
	if (!p->dialog)
		return;
	const AeronFolderDialogStatus status =
		Aeron_PollFolderDialog(p->dialog, path, sizeof path, error, sizeof error);
	if (status == AERON_FOLDER_DIALOG_WAITING)
		return;
	Aeron_DestroyFolderDialog(p->dialog);
	p->dialog = NULL;
	if (status == AERON_FOLDER_DIALOG_SELECTED) {
		snprintf(browse_field(p->browse), XWA_MODS_PATH_CAP, "%s", path);
		p->authoring_edited |= p->browse == XWA_MODS_BROWSE_AUTHORING;
		p->checked = 0;
		if (p->browse == XWA_MODS_BROWSE_MODS_LIST)
			XwaModsPage_ApplyModsFolder();
	} else if (status == AERON_FOLDER_DIALOG_ERROR) {
		char* message = p->browse == XWA_MODS_BROWSE_MODS_LIST ? p->mods_folder_message : p->check_message;
		snprintf(message, 512, "The folder window couldn't open: %s", error);
	}
}

int XwaModsPage_PathRow(const char* label, XwaModsBrowse browse, const char* title) {
	const uint32_t result =
		AeronUi_InputTextWithAction(g_xwaModsPage.ui, label, browse_field(browse), XWA_MODS_PATH_CAP,
									AERON_UI_INPUT_TEXT_NONE, "Browse...");
	if (result & AERON_UI_INPUT_TEXT_ACTION_ACTIVATED)
		open_dialog(browse, title);
	return (result & AERON_UI_INPUT_TEXT_CHANGED) != 0;
}

/* ---- The list ----------------------------------------------------------- */

/* The short status at the right of a mod's row. The details below the list
   say the whole problem. */
static const char* mod_status(const XwaModEntry* entry, int running, int chosen) {
	switch (entry->state) {
		case XWA_MOD_UNFINISHED:
			return "Incomplete";
		case XWA_MOD_OTHER_VERSION:
			return entry->imported && entry->version > XWA_MOD_FORMAT_VERSION ? "Too New" : "Outdated";
		case XWA_MOD_UNREADABLE:
			return "Damaged";
		case XWA_MOD_READY:
			break;
	}
	return running ? "In Use" : chosen ? "Next Start" : NULL;
}

/* Whether `entry` is the mod in folder `path`, where "" is the original
   game. */
static int is_mod(const XwaModEntry* entry, const char* path) {
	return entry->path[0] ? path[0] && XwaModList_SamePath(entry->path, path) : !path[0];
}

static void free_list(void) {
	XwaModsPage* p = &g_xwaModsPage;
	XwaModList_Free(&p->mods);
	free(p->mod_items);
	free(p->mod_imported);
	p->mod_items = NULL;
	p->mod_imported = NULL;
}

static void scan_mods(void) {
	XwaModsPage* p = &g_xwaModsPage;
	char current[XWA_MODS_PATH_CAP] = "";
	free_list();
	p->mod_chosen = SIZE_MAX;
	p->mod_running = SIZE_MAX;
	p->mods_notice[0] = '\0';
	p->mods_scanned = 1;
	if (!XwaModList_Scan(p->saved_mods_folder, p->config.mod, &p->mods)) {
		snprintf(p->mods_message, sizeof p->mods_message, "OpenXWA can't list the mods. Try Refresh.");
		return;
	}
	p->mod_items = calloc(p->mods.count, sizeof *p->mod_items);
	p->mod_imported = calloc(p->mods.count, sizeof *p->mod_imported);
	if (!p->mod_items || !p->mod_imported) {
		free_list();
		return;
	}
	/* The mod that OpenXWA uses at its next start, as XwaSetup_SetModRoot
	   picks it without --mod. */
	if (p->config_ok && p->config.use_mod)
		snprintf(current, sizeof current, "%s", p->config.mod);
	for (size_t i = 0; i < p->mods.count; ++i) {
		const XwaModEntry* entry = &p->mods.entries[i];
		const int chosen = p->config_ok && is_mod(entry, current);
		const int running = is_mod(entry, p->running_mod);
		if (chosen)
			p->mod_chosen = i;
		if (running)
			p->mod_running = i;
#if XWA_HAS_MOD_IMPORT
		p->mod_imported[i] = entry->path[0] && XwaModImport_IsImported(entry->path);
#endif
		p->mod_items[i] = (AeronUiListItem) {
			.id = i,
			.label = entry->name,
			.detail = mod_status(entry, running, chosen && !running),
			/* The mod in use stands out. */
			.flags = entry->problem[0] ? AERON_UI_LIST_ITEM_DIMMED
					 : running         ? AERON_UI_LIST_ITEM_ACCENT
									   : AERON_UI_LIST_ITEM_NONE,
		};
	}
	/* The list leaves out a configured mod without a mod.yaml, such as one
	   whose folder was deleted. */
	if (current[0] && p->mod_chosen == SIZE_MAX)
		snprintf(p->mods_notice, sizeof p->mods_notice,
				 "The chosen mod is missing from %s, so OpenXWA starts with the original game.", current);
	/* The list opens on the mod in use. */
	if (p->mod_selected >= p->mods.count)
		p->mod_selected = p->mod_running;
}

#if XWA_HAS_MOD_IMPORT
static int delete_thread(void* user) {
	XwaModsPage* p = user;
	Aeron_AtomicStore(&p->delete_ok, XwaModImport_Delete(p->delete_path));
	Aeron_AtomicStore(&p->delete_done, 1);
	return 0;
}

static void start_delete(void) {
	XwaModsPage* p = &g_xwaModsPage;
	const XwaModEntry* entry = &p->mods.entries[p->mod_selected];
	snprintf(p->delete_path, sizeof p->delete_path, "%s", entry->path);
	snprintf(p->delete_name, sizeof p->delete_name, "%s", entry->name);
	Aeron_AtomicStore(&p->delete_done, 0);
	Aeron_AtomicStore(&p->delete_ok, 0);
	p->delete_thread = Aeron_ThreadCreate("mod delete", delete_thread, p);
	if (p->delete_thread)
		snprintf(p->mods_message, sizeof p->mods_message, "Deleting %s...", p->delete_name);
	else
		snprintf(p->mods_message, sizeof p->mods_message, "%s couldn't be deleted.", p->delete_name);
}
#endif

/* Joins the delete thread once it has finished. */
static void finish_delete(void) {
	XwaModsPage* p = &g_xwaModsPage;
	char error[512];
	if (!p->delete_thread || !Aeron_AtomicLoad(&p->delete_done))
		return;
	Aeron_ThreadJoin(p->delete_thread);
	p->delete_thread = NULL;
	if (!Aeron_AtomicLoad(&p->delete_ok)) {
		snprintf(p->mods_message, sizeof p->mods_message,
				 "Some files of %s couldn't be deleted. Close any program that uses them and try again, or "
				 "delete the folder with Show Folder.",
				 p->delete_name);
	} else if (p->config_ok && XwaModList_SamePath(p->config.mod, p->delete_path)) {
		/* OpenXWA would warn that the mod is missing at every start. */
		if (XwaInstallPaths_SaveMod(Aeron_GetVfs(), "", 0, error, sizeof error)) {
			snprintf(p->mods_message, sizeof p->mods_message,
					 "%s was deleted. OpenXWA starts with the original game next time.", p->delete_name);
			XwaModsPage_LoadConfig();
		} else {
			snprintf(p->mods_message, sizeof p->mods_message,
					 "%s was deleted, but OpenXWA can't save the mod choice: %s", p->delete_name, error);
		}
	} else {
		snprintf(p->mods_message, sizeof p->mods_message, "%s was deleted.", p->delete_name);
	}
	p->mods_scanned = 0;
	p->mod_selected = SIZE_MAX;
	/* The form checks the mod folder again. */
	p->checked = 0;
}

static void draw_list(void) {
	XwaModsPage* p = &g_xwaModsPage;
	AeronUiContext* ui = p->ui;
	const AeronUiTheme* theme = AeronUi_GetTheme(ui);
	if (p->mods_folder_edit_us && Aeron_NowUs() - p->mods_folder_edit_us >= MODS_FOLDER_DELAY_US)
		XwaModsPage_ApplyModsFolder();
	if (!p->mods_scanned)
		scan_mods();
	if (p->config_message[0])
		AeronUi_Error(ui, p->config_message);
	if (XwaModsPage_PathRow("Mods Folder", XWA_MODS_BROWSE_MODS_LIST, "Choose the Mods Folder")) {
		p->mods_folder_edit_us = Aeron_NowUs();
		p->mods_folder_message[0] = '\0';
		p->checked = 0;
	}
	if (p->mods_folder_message[0])
		AeronUi_Error(ui, p->mods_folder_message);
	if (p->mods_notice[0])
		AeronUi_Error(ui, p->mods_notice);

	/* Below the list: the name, folder, problem and advice of the selected
	   mod, the restart line and a message. A long folder takes more lines. */
	const float line = AeronUi_MeasureHelpHeight(ui, "X", 0.0f) + theme->item_spacing;
	const float details = theme->row_height + theme->item_spacing + 5.0f * line;
	AeronUi_ListBox(ui, "Mods", p->mod_items, p->mod_items ? p->mods.count : 0, &p->mod_selected,
					fmaxf(120.0f, XwaModsPage_ContentHeight() - XWA_MODS_BUTTON_ROWS_REF - details));
	const XwaModEntry* selected =
		p->mod_items && p->mod_selected < p->mods.count ? &p->mods.entries[p->mod_selected] : NULL;
	/* The original game's folder is the game data folder. */
	const char* folder = !selected ? "" : selected->path[0] ? selected->path : p->game;
	if (selected) {
		char text[XWA_MODS_PATH_CAP + 64];
		const char* advice = XwaModList_Advice(selected, XWA_MOD_ADVICE_MODS_TAB);
		AeronUi_Label(ui, selected->name);
		if (folder[0]) {
			snprintf(text, sizeof text, "Folder: %s", folder);
			AeronUi_Help(ui, text);
		}
		if (selected->problem[0]) {
			snprintf(text, sizeof text, "%s%s%s", selected->problem, advice ? " " : "", advice ? advice : "");
			AeronUi_Error(ui, text);
		}
	}
	/* The choice applies at the next start, so say so only when it changed.
	   With --mod, the next start without it uses the choice. */
	if (!p->launch_override && p->mod_chosen < p->mods.count && p->mod_chosen != p->mod_running) {
		char text[320];
		const XwaModEntry* chosen = &p->mods.entries[p->mod_chosen];
		if (chosen->path[0])
			snprintf(text, sizeof text, "Restart OpenXWA to play with %s.", chosen->name);
		else
			snprintf(text, sizeof text, "Restart OpenXWA to play the original game.");
		/* In the accent color, like a mod's problem, so it isn't missed. */
		AeronUi_Error(ui, text);
	}
	if (p->mods_message[0])
		AeronUi_Help(ui, p->mods_message);

	AeronUi_Spacer(ui, fmaxf(0.0f, XwaModsPage_ContentHeight() - XWA_MODS_BUTTON_ROWS_REF));
	AeronUi_Separator(ui);
	const int idle = !p->delete_thread && !p->thread;
#if XWA_HAS_MOD_IMPORT
	AeronUi_BeginColumns(ui, 5, NULL);
	if (AeronUi_ButtonEnabled(ui, "Import a Mod...", idle && !p->dialog)) {
		/* The import goes in the mods folder of the field. */
		if (p->mods_folder_edit_us)
			XwaModsPage_ApplyModsFolder();
		p->view = XWA_MODS_VIEW_FORM;
		p->checked = 0;
	}
	AeronUi_NextColumn(ui);
#else
	AeronUi_BeginColumns(ui, 3, NULL);
#endif
	const int usable = idle && p->config_ok && selected && p->mod_selected != p->mod_chosen &&
					   selected->state == XWA_MOD_READY;
	if (AeronUi_ButtonEnabled(ui, "Use This Mod", usable))
		XwaModsPage_UseMod(selected->path, p->mods_message, sizeof p->mods_message);
	AeronUi_NextColumn(ui);
	if (AeronUi_ButtonEnabled(ui, "Show Folder", folder[0] != '\0') && !Aeron_OpenFolder(folder))
		snprintf(p->mods_message, sizeof p->mods_message, "Couldn't open %s.", folder);
	AeronUi_NextColumn(ui);
#if XWA_HAS_MOD_IMPORT
	/* Only a mod that the importer made, as it can be imported again, and
	   not the one in use, whose files OpenXWA reads. */
	const int deletable =
		idle && selected && p->mod_imported[p->mod_selected] && p->mod_selected != p->mod_running;
	if (AeronUi_ButtonEnabled(ui, "Delete...", deletable))
		p->delete_open = 1;
	AeronUi_NextColumn(ui);
#endif
	if (AeronUi_ButtonEnabled(ui, "Refresh", idle)) {
		p->mods_message[0] = '\0';
		p->mods_scanned = 0;
	}
	AeronUi_EndColumns(ui);
}

/* ---- The page ----------------------------------------------------------- */

void XwaModsPage_Init(AeronUiContext* ui, const XwaModsPageSetup* setup) {
	XwaModsPage* p = &g_xwaModsPage;
	memset(p, 0, sizeof *p);
	p->ui = ui;
	snprintf(p->game, sizeof p->game, "%s", setup && setup->game_data ? setup->game_data : "");
	snprintf(p->running_mod, sizeof p->running_mod, "%s",
			 setup && setup->running_mod ? setup->running_mod : "");
	p->launch_override = setup && setup->launch_override;
	snprintf(p->resources, sizeof p->resources, "%s", Aeron_ResourceRoot());
	XwaModsPage_LoadConfig();
	snprintf(p->mods_folder, sizeof p->mods_folder, "%s", p->saved_mods_folder);
	p->log_selected = SIZE_MAX;
	p->failure_selected = SIZE_MAX;
	p->mod_selected = SIZE_MAX;
}

void XwaModsPage_Shutdown(void) {
	XwaModsPage* p = &g_xwaModsPage;
#if XWA_HAS_MOD_IMPORT
	XwaModsImport_Shutdown();
#endif
	/* A delete can't stop halfway, so it finishes first. */
	if (p->delete_thread)
		Aeron_ThreadJoin(p->delete_thread);
	p->delete_thread = NULL;
	if (p->dialog)
		Aeron_DestroyFolderDialog(p->dialog);
	p->dialog = NULL;
	free_list();
}

void XwaModsPage_Open(void) {
	XwaModsPage* p = &g_xwaModsPage;
	XwaModsPage_LoadConfig();
	p->mods_scanned = 0;
	/* The list and the form share the mods folder, so an edit in the form
	   changes the list too. */
	if (p->view == XWA_MODS_VIEW_LIST)
		XwaModsPage_ApplyModsFolder();
}

void XwaModsPage_Update(void) {
	XwaModsPage* p = &g_xwaModsPage;
	poll_dialog();
#if XWA_HAS_MOD_IMPORT
	XwaModsImport_Update();
#endif
	finish_delete();
	if (p->quit_after_delete && !p->delete_thread) {
		p->quit_after_delete = 0;
		Aeron_RequestQuit();
	}
}

void XwaModsPage_Draw(void) {
	XwaModsPage* p = &g_xwaModsPage;
	if (p->view == XWA_MODS_VIEW_LIST) {
		draw_list();
		return;
	}
#if XWA_HAS_MOD_IMPORT
	XwaModsImport_Draw();
#endif
}

void XwaModsPage_DrawModals(void) {
	XwaModsPage* p = &g_xwaModsPage;
	AeronUiContext* ui = p->ui;
	const AeronUiWindowDesc desc = { .width_ref = 900.0f, .centered = 1 };
#if XWA_HAS_MOD_IMPORT
	if (p->delete_open && AeronUi_BeginModal(ui, "Delete the Mod", &p->delete_open, &desc)) {
		/* The selection can't change while the modal is open. */
		const XwaModEntry* entry = p->mod_selected < p->mods.count ? &p->mods.entries[p->mod_selected] : NULL;
		/* The name and the folder on their own lines, as a folder can be
		   long. */
		if (entry) {
			AeronUi_Help(ui, "This deletes the mod and its folder:");
			AeronUi_Label(ui, entry->name);
			AeronUi_Help(ui, entry->path);
			AeronUi_Spacer(ui, 12.0f);
			AeronUi_Help(ui, "To get it back, import it again from its XWAU or TFTC folder.");
			if (p->mod_selected == p->mod_chosen)
				AeronUi_Help(ui, "OpenXWA then starts with the original game.");
			AeronUi_Spacer(ui, 12.0f);
		}
		AeronUi_BeginColumns(ui, 2, NULL);
		if (AeronUi_Button(ui, "Cancel"))
			p->delete_open = 0;
		AeronUi_NextColumn(ui);
		if (AeronUi_ButtonEnabled(ui, "Delete", entry != NULL)) {
			p->delete_open = 0;
			start_delete();
		}
		AeronUi_EndColumns(ui);
		AeronUi_EndModal(ui);
	}
	XwaModsImport_DrawModals();
#else
	(void)ui;
	(void)desc;
#endif
}

bool XwaModsPage_Busy(void) {
	const XwaModsPage* p = &g_xwaModsPage;
	return p->thread != NULL || p->delete_thread != NULL;
}

const char* XwaModsPage_BusyMessage(void) {
	const XwaModsPage* p = &g_xwaModsPage;
	if (p->thread)
		return "You can close this window when the import ends.";
	if (p->delete_thread)
		return "You can close this window when the mod is deleted.";
	return NULL;
}

void XwaModsPage_CancelImport(void) {
#if XWA_HAS_MOD_IMPORT
	XwaModsImport_Cancel();
#endif
}

bool XwaModsPage_RequestQuit(void) {
	XwaModsPage* p = &g_xwaModsPage;
#if XWA_HAS_MOD_IMPORT
	if (p->thread) {
		XwaModsImport_RequestQuit();
		return true;
	}
#endif
	if (p->delete_thread) {
		p->quit_after_delete = 1;
		snprintf(p->mods_message, sizeof p->mods_message, "OpenXWA quits when %s is deleted.",
				 p->delete_name);
		return true;
	}
	return false;
}
