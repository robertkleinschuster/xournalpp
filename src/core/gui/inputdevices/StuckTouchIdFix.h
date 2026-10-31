/*
 * Xournal++
 *
 * Keeps GDK from ignoring touches whose pointer id was used by a cancelled touch (Windows)
 *
 * @license GNU GPLv2 or later
 */

#pragma once

namespace StuckTouchIdFix {

/**
 * GDK 3 on Windows ignores all further messages of a touch that Windows cancels or reports without confidence (e.g. a
 * palm while writing). It only forgets that pointer id again on a WM_POINTERLEAVE, and only one of the several entries
 * it adds per cancelled message. Windows reuses touch pointer ids, so a later finger with that id is silently dropped:
 * every gesture then needs one finger more (two to scroll, three to zoom), until the application is restarted.
 *
 * This installs a GDK event filter that counts how often GDK marks a touch id as ignored and, when a new, valid touch
 * starts with that id, lets GDK forget it again before GDK handles the touch. Does nothing on other platforms.
 */
void install();

}  // namespace StuckTouchIdFix
