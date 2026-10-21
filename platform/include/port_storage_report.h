#pragma once

// A startup log of where the app's disk space goes: one line per top-level entry of the Android
// internal storage, the Remastered import root and the data folder, so a full device can be
// diagnosed from the log alone.

namespace PortStorageReport {

// Walks the folders on a detached background thread and logs
// "port: storage: <path> <MiB> MiB (<files> files)" per entry and a "total" line. Never blocks
// startup, never follows symlinks, never throws.
void StartLogging();

} // namespace PortStorageReport
