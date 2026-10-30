# Native code finds these by name (GrooveJni.cpp: RegisterNatives + GetMethodID). Without these rules R8 would
# rename or remove them and System.loadLibrary("groove") would fail in JNI_OnLoad.
-keep class app.groove.engine.GrooveNative { native <methods>; }
-keepclassmembers class app.groove.engine.GrooveEngine {
    private void onNativeState(int, long);
    private void onNativeTrackStarted(long);
    private void onNativeTrackFailed(long, int, java.lang.String);
    private void onNativeQueueEnded();
    private void onNativeOutput(int, int, boolean, boolean, boolean);
}
-keepclassmembers class app.groove.engine.GrooveLibrary {
    private void onNativeScanProgress(long, long, long, long, long, boolean);
}
-keep class app.groove.engine.SafFileOpener { public int openFd(java.lang.String); }
-keepnames class app.groove.engine.GrooveEngine
-keepnames class app.groove.engine.GrooveLibrary
