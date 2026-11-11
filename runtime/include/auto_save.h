#pragma once

// RE4 auto-save: every room entry the game refreshes its continue checkpoint (gameStageInit ->
// cGameSave::save(pSaveData, -1)). This writes that checkpoint as a normal save file in one fixed
// slot (FILE 20 by default), so after a crash the player loads it from the title's Load Game and
// starts at the door of the last room. RE4_AUTO_SAVE=0 turns it off; RE4_AUTO_SAVE_SLOT=1..20
// picks another slot.
namespace AutoSave {

// Once a retrace on the guest thread.
void OnRetrace();

// The session's first auto-save has been written and its notice not shown yet: returns the slot's
// FILE number (1-20) once, else 0. The overlay warns that hand saves to that slot get overwritten.
// Any thread.
int TakeNotice();

} // namespace AutoSave
