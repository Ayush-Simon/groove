package app.groove.engine;

/** Native SQLite library index (next phase). Present so the scan-progress callback bound in JNI_OnLoad exists. */
public final class GrooveLibrary {
    private GrooveLibrary() {}

    @SuppressWarnings("unused")
    private void onNativeScanProgress(long seen, long added, long updated, long removed, long failed, boolean finished) { }
}
