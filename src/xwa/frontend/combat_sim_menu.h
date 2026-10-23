#ifndef XWA_FRONTEND_COMBAT_SIM_MENU_H
#define XWA_FRONTEND_COMBAT_SIM_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

extern int g_combatSimModelLimitsRequested;

int CombatSimMenu_Update(int frameCounter);
int CombatSimMenu_Exit(int frameCounter);

#ifdef __cplusplus
}
#endif

#endif
