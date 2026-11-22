# Release (R8) keep rules.
# SDL's Java glue is called from native code by class and method name (JNI), and the overlay reaches its
# package-private members by reflection (SdlVirtualGamepad).
-keep class org.libsdl.app.** { *; }
# GameActivity and the overlay are entered from SDL / the manifest; keep their names for crash traces.
-keepnames class org.recompia.pkp.** { *; }
-dontwarn org.libsdl.app.**
