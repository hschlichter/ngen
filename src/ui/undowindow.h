#pragma once

class UndoStack;
class USDScene;
class SceneUpdater;

// Read-only visualization of the undo/redo stacks. Buttons trigger undo/redo
// through the scene updater (same path as Ctrl+Z / Ctrl+Shift+Z).
void drawUndoWindow(bool& show, SceneUpdater& sceneUpdater, const USDScene& scene);
