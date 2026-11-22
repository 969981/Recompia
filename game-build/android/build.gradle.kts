// AGP 9 compiles Kotlin itself (built-in Kotlin): no kotlin-android plugin.
plugins {
    id("com.android.application") version "9.3.1" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.4.10" apply false
}
