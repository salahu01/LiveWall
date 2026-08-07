# The service and the settings activity are instantiated by name from the
# manifest, so R8 cannot see the reference.
-keep class com.fegno.livewall.app.LiveWallService { *; }
-keep class com.fegno.livewall.ui.SettingsActivity { *; }
