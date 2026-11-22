import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

// SDL's Java glue is compiled straight from the SDL source tree that produced libSDL3.so
// (nothing third-party is stored in this repo). Lookup: -P / gradle.properties, then local.properties.
val localProps = Properties().apply {
    rootProject.file("local.properties").takeIf { it.isFile }?.inputStream()?.use { load(it) }
}
val sdlJavaDir: String = providers.gradleProperty("recompia.sdlJavaDir").orNull
    ?: localProps.getProperty("recompia.sdlJavaDir")
    ?: throw GradleException(
        "recompia.sdlJavaDir is not set. Point it at <SDL source>/android-project/app/src/main/java " +
            "(same SDL release as libSDL3.so) in game-build/android/local.properties, or run tools/android/run/sync-libs.sh."
    )
if (!file("$sdlJavaDir/org/libsdl/app/SDLActivity.java").isFile) {
    throw GradleException("recompia.sdlJavaDir=$sdlJavaDir does not contain org/libsdl/app/SDLActivity.java")
}

// Release signing: a keystore kept OUTSIDE every repository and build folder. Its folder comes from the git-ignored local.properties
// (recompia.signing.dir) or the environment (RECOMPIA_SIGNING_DIR) and holds release.jks and passwords.txt (storePassword, keyPassword,
// keyAlias). Without it a release build FAILS, unless -PallowDebugSignedRelease=true asks for the local debug key on purpose
// (-Precompia.signing.dir= with an empty value hides the folder for one build).
val signingDir: String? = (System.getenv("RECOMPIA_SIGNING_DIR") ?: providers.gradleProperty("recompia.signing.dir").orNull
    ?: localProps.getProperty("recompia.signing.dir"))?.takeIf { it.isNotBlank() }
val allowDebugSignedRelease = providers.gradleProperty("allowDebugSignedRelease").orNull == "true"
val signingProps: Properties? = signingDir?.let { d -> file("$d/passwords.txt").takeIf { it.isFile && file("$d/release.jks").isFile }
    ?.let { f -> Properties().apply { f.inputStream().use { load(it) } } } }

// The release version (-Precompia.versionName=x.y.z, tools/android/release/build-apk.sh): the app's version name, and its version
// code as major*10^6 + minor*10^4 + patch*100. Not given: the version below.
val releaseVersion: List<Int>? = providers.gradleProperty("recompia.versionName").orNull
    ?.let { Regex("""^(\d+)\.(\d+)\.(\d+)$""").find(it) }?.groupValues?.drop(1)?.map { it.toInt() }

android {
    namespace = "org.recompia.pkp"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "org.recompia.pkp"
        minSdk = 30
        targetSdk = 35
        versionCode = releaseVersion?.let { it[0] * 1_000_000 + it[1] * 10_000 + it[2] * 100 } ?: 16000200   // major*10^6 + minor*10^4 + patch*100 + sub
        versionName = releaseVersion?.joinToString(".") ?: "0.16.0.2"
        // Test entry points (fake builder, "Build from exefs only", shader test size): false here so R8 drops them and the
        // resource shrinker drops their strings; true for debug and internalTest.
        buildConfigField("boolean", "TEST_ENTRY_POINTS", "false")
        ndk { abiFilters += "arm64-v8a" }
    }

    signingConfigs {
        if (signingProps != null) create("release") {
            storeFile = file("$signingDir/release.jks")
            storePassword = signingProps.getProperty("storePassword")
            keyAlias = signingProps.getProperty("keyAlias")
            keyPassword = signingProps.getProperty("keyPassword")
        }
    }

    buildTypes {
        debug {
            buildConfigField("boolean", "TEST_ENTRY_POINTS", "true")
        }
        release {
            // Small APK for nearly full phones: R8 + resource shrinking (the debug APK carries ~60 MB of unshrunk dex).
            // Keep rules for SDL's JNI glue and the overlay's reflection into it: proguard-rules.pro.
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
        }
        // INTERNAL TEST ONLY, never shipped: exactly the release build (R8, resource shrinking, same keep rules, not debuggable)
        // plus the debug source set's test entry points (fake builder, "Build from exefs only", shader test size) and its manifest
        // (show over the lock screen for unattended adb runs), so the real build path can be run with minified code on a phone
        // that has no room for the romfs.
        create("internalTest") {
            initWith(getByName("release"))
            signingConfig = signingConfigs.getByName("debug")   // never shipped: the debug key, so it installs over test installs
            buildConfigField("boolean", "TEST_ENTRY_POINTS", "true")
            proguardFile("internal-test-rules.pro")
            matchingFallbacks += "release"
        }
    }
    sourceSets["internalTest"].kotlin.directories += "src/debug/java"
    sourceSets["internalTest"].manifest.srcFile("src/debug/AndroidManifest.xml")

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    sourceSets["main"].java.srcDir(sdlJavaDir)
    // Repo files the UI reads at run time, copied at build time so they never go stale (see uiAssets below)
    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("generated/uiAssets").get().asFile)
    // the prompt icon pack is copied out by size (AppViewModel.ensurePromptIcons): stored uncompressed, so openFd gives its length
    androidResources { noCompress += "pak" }

    packaging {
        jniLibs {
            // Extract native libs to nativeLibraryDir: on-device tool executables named lib*.so must be exec'able.
            useLegacyPackaging = true
        }
    }

    lint {
        abortOnError = false
    }
}

// Common dependencies of the UI and the overlay.
dependencies {
    implementation(platform("androidx.compose:compose-bom:2025.10.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    debugImplementation("androidx.compose.ui:ui-tooling")

    implementation("androidx.core:core-ktx:1.17.0")
    implementation("androidx.appcompat:appcompat:1.7.1")
    implementation("androidx.activity:activity-ktx:1.11.0")
    implementation("androidx.activity:activity-compose:1.11.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.4")
    implementation("androidx.lifecycle:lifecycle-viewmodel-ktx:2.9.4")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.9.4")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.9.4")
    implementation("androidx.lifecycle:lifecycle-service:2.9.4")
    implementation("androidx.navigation:navigation-compose:2.9.5")
    implementation("androidx.documentfile:documentfile:1.1.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")
}

// The settings schemas, the supported dump's hashes and the credits/legal text, straight from the repo (the UI reads them).
val uiAssets = tasks.register<Copy>("uiAssets") {
    from(
        rootProject.file("../../runtime/docs/pkp_ini_schema.json"),
        rootProject.file("../../runtime/docs/controls_schema.json"),
        rootProject.file("../release_b/dump_hashes.json"),
        rootProject.file("../../CREDITS.md"),
        rootProject.file("../../LEGAL.md"),
    )
    into(layout.buildDirectory.dir("generated/uiAssets"))
    // the button prompt icon pack (Kenney Input Prompts, CC0, runtime/assets/prompts): copied to <data root>/assets/prompts at start
    into("prompts") { from(rootProject.file("../../runtime/assets/prompts")) { include("prompt_icons.pak") } }
    // licence texts the About page shows for what the APK carries (font, SDL, LLVM's libc++ / clang / lld, bcdec in the runtime)
    into("licenses") {
        from(rootProject.file("../../tools/release/licenses")) {
            include("LICENSE.Inter.OFL-1.1.txt", "LICENSE.SDL3.zlib.txt", "LICENSE.libcxxabi.Apache-2.0-LLVM-exception.txt")
            // libpkp_rt.so's own third-party code, as tools/release/licenses/README.txt lists it for pkp_rt (the launcher's are left out)
            include("LICENSE.Dear-ImGui.MIT.txt", "LICENSE.volk.MIT.txt", "LICENSE.VulkanMemoryAllocator.MIT.txt",
                    "LICENSE.Vulkan-Headers.Apache-2.0.txt", "LICENSE.Vulkan-Headers.MIT.txt", "LICENSE.Vulkan-Headers.md",
                    "LICENSE.zstd.BSD-3-Clause.txt", "LICENSE.miniz.MIT.txt", "LICENSE.Modtopia.MIT.txt", "LICENSE.SMAA.MIT.txt",
                    "LICENSE.NVIDIA-Image-Scaling.MIT.txt", "LICENSE.FidelityFX-FSR1.MIT.txt", "LICENSE.FidelityFX-API.MIT.txt",
                    "LICENSE.Streamline-headers.MIT.txt", "LICENSE.nvn-header-h1k421.MIT.txt", "LICENSE.astc-encoder.Apache-2.0.txt",
                    "LICENSE.bc7enc_rdo.MIT-Apache-2.0.txt", "LICENSE.DejaVu-fonts.Bitstream-Vera.txt",
                    "LICENSE.FontAwesome-Free.CC-BY-4.0-MIT-OFL.txt", "LICENSE.pokopia-native.MIT.txt", "LICENSE.pokopia-coop.MIT.txt",
                    "LICENSE.totk-pc-recomp.MIT.txt", "LICENSE.libnx.ISC.txt")
        }
        // what the runtime takes from which project, with the licence texts it quotes (Ryujinx and ryubing among them)
        from(rootProject.file("../../runtime/THIRD_PARTY_NOTICES.md"))
        // Qualcomm Snapdragon Game Super Resolution (BSD-3-Clause), in the runtime
        from(rootProject.file("../../runtime/third_party/qualcomm_sgsr")) {
            include("LICENSE*")
            rename { "LICENSE.Snapdragon-GSR.BSD-3-Clause.txt" }
        }
        from(rootProject.file("../../runtime/assets/prompts")) { include("LICENSE-Kenney-InputPrompts.txt") }
    }
    // bcdec keeps its licence at the end of its header (MIT or public domain); plain File values for the configuration cache
    val bcdecHeader = rootProject.file("../../runtime/src/gfx/nvn/third_party/bcdec/bcdec.h")
    val bcdecLicence = layout.buildDirectory.file("generated/uiAssets/licenses/LICENSE.bcdec.MIT-Unlicense.txt").get().asFile
    inputs.file(bcdecHeader)
    doLast {
        val lic = bcdecHeader.readText().substringAfterLast("/* LICENSE:", "").substringBefore("*/").trim()
        if (lic.isNotEmpty()) bcdecLicence.writeText(lic + "\n")
    }
}
tasks.named("preBuild") { dependsOn(uiAssets) }

// a release signed with the debug key never goes out by mistake: without the signing folder, the release build stops here
val releaseKeyMissing = signingProps == null && !allowDebugSignedRelease
val checkReleaseSigning = tasks.register("checkReleaseSigning") {
    val missing = releaseKeyMissing   // a plain value for the configuration cache
    doLast {
        if (missing) throw GradleException("No release signing folder (recompia.signing.dir in local.properties or " +
            "RECOMPIA_SIGNING_DIR). Pass -PallowDebugSignedRelease=true to sign this release with the debug key on purpose.")
    }
}
tasks.matching { it.name == "preReleaseBuild" }.configureEach { dependsOn(checkReleaseSigning) }
