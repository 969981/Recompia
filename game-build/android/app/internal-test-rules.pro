# internalTest build type only: the debug fake builder is reached by reflection (FakeBuilder in BuildService.kt).
-keep class org.recompia.pkp.build.FakeBuilderImpl { *; }
