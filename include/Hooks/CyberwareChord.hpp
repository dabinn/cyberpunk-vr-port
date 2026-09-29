#pragma once

namespace cvr::input {
// Queues one private F18 action when the current game/UI context accepts the
// iconic-cyberware shortcut. Chord recognition and re-arming live in the XR
// controller state machine.
bool RequestCyberwareChord();
// Called by the game's input poll. Delivers a single direct native-action key
// only while this process owns the foreground; never types into another app.
void DispatchCyberwareChord();
}
