import java.util.Properties
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// The port's tree: this Gradle project lives in quest/ at its root.
val portRoot: File = rootProject.projectDir.parentFile

// Generated inputs, so nothing from android/, launcher/ or the repository root is
// committed a second time under quest/.
val generatedJava = layout.buildDirectory.dir("generated/sdlJava")
val generatedAssets = layout.buildDirectory.dir("generated/portAssets")
val generatedLicenses = layout.buildDirectory.dir("generated/licenses")
val generatedRes = layout.buildDirectory.dir("generated/launcherRes")
val generatedCannonAssets = layout.buildDirectory.dir("generated/cannonAssets")

// The native build. The first list is upstream's phone build (android/app/build.gradle,
// cmakeArguments): keep it in step, Build-Quest.ps1 warns when upstream's changes.
val upstreamCmakeArguments = listOf(
    "-DANDROID_STL=c++_static",
    "-DMP_BUILD_TESTS=OFF",
    "-DMP_ANDROID_NOD_STUB=OFF",
    "-DAURORA_SDL3_PROVIDER=vendor",
    "-DAURORA_SDL3_LINKAGE=static",
    "-DAURORA_DAWN_PROVIDER=auto",
    "-DAURORA_DAWN_LINKAGE=static",
    "-DAURORA_NOD_PROVIDER=vendor",
    "-DAURORA_NOD_LINKAGE=static",
    "-DRust_CARGO_TARGET=aarch64-linux-android",
)
val questCmakeArguments = listOf(
    // Optimised native code in every variant: AGP builds debug at -O0, which no
    // headset can play.
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
    "-DMP_ENABLE_OPENXR=ON",
    // OpenSSL's Android build needs make and a Unix PATH. Without it the Quest
    // build refuses Archipelago's wss:// servers and the Remastered NSP import.
    "-DMP_ALLOW_NO_TLS=ON",
    // The launcher panel's native half (launcher/jni): the PC launcher's core.
    "-DMP_BUILD_QUEST_LAUNCHER=ON",
)
// PrimedGun's patched Dawn (quest/Build-QuestDawn.ps1: Vulkan multiview, so one render
// pass draws both eyes), as the package Build-Quest.ps1 passes with -PquestDawnPackage;
// without it, aurora's stock prebuilt and per-eye passes.
val questDawnPackage = providers.gradleProperty("questDawnPackage").orNull?.takeIf { it.isNotBlank() }
val dawnCmakeArguments = questDawnPackage?.let {
    listOf(
        "-DAURORA_DAWN_PROVIDER=package",
        "-DAURORA_DAWN_PACKAGE_URL=" + File(it).absolutePath.replace('\\', '/'),
    )
} ?: emptyList()
// Short, because nod's Rust build nests deep below it and Windows paths are finite.
val nativeStagingDir = File(portRoot, "build/quest-cxx")

// Release signing, as upstream's phone build does it: a key of this project's own
// from quest/keystore.properties (untracked) or the environment, all four values
// or none. Signing a release with the shared debug key has to be asked for with
// -PquestDebugSigning=true, because Android treats a later change of key as another
// app: the debug-signed install would have to be removed, and its data with it.
val signingProperties = Properties().apply {
    val file = rootProject.file("keystore.properties")
    if (file.exists()) {
        file.inputStream().use { load(it) }
    }
}

fun signingValue(key: String, environmentName: String): String? =
    signingProperties.getProperty(key)?.trim()?.takeIf { it.isNotEmpty() }
        ?: System.getenv(environmentName)?.trim()?.takeIf { it.isNotEmpty() }

val keystorePath = signingValue("storeFile", "PRIMEDGUN_QUEST_KEYSTORE")
    ?.let { if (File(it).isAbsolute) it else rootProject.file(it).absolutePath }
val keystorePassword = signingValue("storePassword", "PRIMEDGUN_QUEST_KEYSTORE_PASSWORD")
val signingKeyAlias = signingValue("keyAlias", "PRIMEDGUN_QUEST_KEY_ALIAS")
val signingKeyPassword = signingValue("keyPassword", "PRIMEDGUN_QUEST_KEY_PASSWORD")
val signingParts = listOf(keystorePath, keystorePassword, signingKeyAlias, signingKeyPassword)
val hasReleaseKey = signingParts.all { it != null }
if (!hasReleaseKey && signingParts.any { it != null }) {
    throw GradleException(
        "release signing is half configured; storeFile, storePassword, keyAlias and keyPassword " +
            "are all needed (quest/keystore.properties or PRIMEDGUN_QUEST_KEY*)."
    )
}
if (hasReleaseKey && !File(keystorePath!!).exists()) {
    throw GradleException("keystore not found at $keystorePath")
}
val allowDebugSigning = providers.gradleProperty("questDebugSigning").orNull?.toBoolean() ?: false

android {
    namespace = "org.primedgun.v2"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    signingConfigs {
        if (hasReleaseKey) {
            create("release") {
                storeFile = file(keystorePath!!)
                storePassword = keystorePassword
                keyAlias = signingKeyAlias
                keyPassword = signingKeyPassword
            }
        }
    }

    defaultConfig {
        applicationId = "org.primedgun.v2"
        // Quest 2 shipped Android 10 (API 29); AHardwareBuffer needs 26.
        minSdk = 29
        targetSdk = 34
        versionCode = 11
        versionName = "2.0.0-beta.6"

        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += upstreamCmakeArguments + questCmakeArguments + dawnCmakeArguments
                targets += listOf("metroid_prime_port", "primedgun_launcher")
            }
        }
    }

    externalNativeBuild {
        cmake {
            // No version pin: Build-Quest.ps1 points cmake.dir at a system CMake
            // (the port needs 3.25+) and puts the SDK's Ninja on PATH.
            path = File(portRoot, "CMakeLists.txt")
            buildStagingDirectory = nativeStagingDir
        }
    }

    buildTypes {
        getByName("debug") {
            isDebuggable = true
        }
        getByName("release") {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = when {
                hasReleaseKey -> signingConfigs.getByName("release")
                allowDebugSigning -> signingConfigs.getByName("debug")
                else -> null
            }
        }
    }

    buildFeatures {
        buildConfig = true
        viewBinding = true
    }

    sourceSets.named("main") {
        java.srcDir(generatedJava)
        assets.srcDir(generatedAssets)
        assets.srcDir(generatedLicenses)
        assets.srcDir(generatedCannonAssets)
        res.srcDir(generatedRes)
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    lint {
        disable += setOf("ChromeOsAbiSupport", "DiscouragedApi")
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

// Signing a release with the debug key is a decision, not a default; see above.
tasks.matching { it.name == "preReleaseBuild" }.configureEach {
    doFirst {
        if (!hasReleaseKey && !allowDebugSigning) {
            throw GradleException(
                "refusing to sign the release with the shared debug key.\n" +
                    "  create this project's own key (see quest/README.md), or, for a build you\n" +
                    "  only sideload, pass -PquestDebugSigning=true (Build-Quest.ps1 does when no\n" +
                    "  key is configured)"
            )
        }
    }
}

// SDL's Java and Aurora's surface class, from upstream's phone project. The Java
// side must be the same SDL release as the native one the game links.
val syncSdlJava by tasks.registering(Sync::class) {
    from(File(portRoot, "android/app/src/main/java")) {
        include("org/libsdl/app/**")
        include("dev/encounter/aurora/**")
    }
    into(generatedJava)
}

// SDLActivity refuses to start on a Java/native version mismatch, with a dialog
// nobody sees inside the headset; fail the build instead.
val checkSdlVersion by tasks.registering {
    val activity = File(portRoot, "android/app/src/main/java/org/libsdl/app/SDLActivity.java")
    val versions = File(portRoot, "extern/aurora/cmake/AuroraDependencyVersions.cmake")
    inputs.files(activity, versions)
    doLast {
        val java = activity.readText()
        val javaVersion = listOf("SDL_MAJOR_VERSION", "SDL_MINOR_VERSION", "SDL_MICRO_VERSION").joinToString(".") {
            Regex("""$it\s*=\s*(\d+)""").find(java)?.groupValues?.get(1) ?: "?"
        }
        val nativeVersion = Regex("""AURORA_SDL3_VERSION\s+"([0-9.]+)"""").find(versions.readText())?.groupValues?.get(1)
        if (nativeVersion != null && javaVersion != nativeVersion) {
            throw GradleException(
                "SDL's Java sources in android/app are $javaVersion but the native SDL is $nativeVersion"
            )
        }
    }
}

// The textures and pipeline cache the game expects in its private folder;
// PrimedGunVrActivity unpacks them, as upstream's MetroidPrimeActivity does.
val syncPortAssets by tasks.registering(Sync::class) {
    into(generatedAssets)
    from(File(portRoot, "platform/include/vr/vr_controller_bindings.inc"))
    from(File(portRoot, "textures")) {
        into("textures")
    }
    // Not upstream's seed (assets/initial_pipeline_cache.db): its rows are the phone and
    // desktop's mono configs, most of them with indexed attributes, which the headset
    // (de-indexed vertices, multiview eye pipelines) never asks for, and it cost ~2249
    // background compiles at every start. PrimedGun's own seed (tools/primedgun_seed.py,
    // built from the Quest's and the PC's caches) holds the configs the fork asks for;
    // aurora keeps the eye pipelines this headset draws with.
    from(File(portRoot, "assets")) {
        include("primedgun_pipeline_cache.db")
    }
}

// PrimedGun's cannon texture slots, which the launcher unpacks and seeds into the
// user folder (launcher/core/cannon_textures.h), as the PC build copies them next
// to PrimedGun.exe.
val syncCannonTextures by tasks.registering(Sync::class) {
    into(generatedCannonAssets)
    from(File(portRoot, "launcher/data/cannon_textures")) {
        into("cannon_textures")
    }
}

// The PC launcher's pictures, under names Android resources accept.
val syncLauncherRes by tasks.registering(Sync::class) {
    into(generatedRes)
    from(File(portRoot, "launcher/assets")) {
        include("*.png")
        into("drawable-nodpi")
        rename { name -> if (name == "PrimedGun.png") "primedgun_logo.png" else "primedgun_" + name.lowercase() }
    }
}

// The terms of everything the APK carries, as upstream's phone build gathers
// them: the port's own, the vendored snapshots, and what the native build fetched.
val syncLicenseNotices by tasks.registering(Sync::class) {
    into(generatedLicenses)
    duplicatesStrategy = DuplicatesStrategy.EXCLUDE
    from(File(portRoot, "LICENSE")) { into("licenses"); rename { "port-license.txt" } }
    from(File(portRoot, "NOTICE")) { into("licenses"); rename { "port-notice.txt" } }
    from(File(portRoot, "extern/aurora/LICENSE")) { into("licenses"); rename { "aurora.txt" } }
    from(File(portRoot, "extern/musyx/LICENSE")) { into("licenses"); rename { "musyx.txt" } }
    listOf(
        "sdl-src", "imgui-src", "fmt-src", "zstd-src", "openxr-src", "dawn_prebuilt-src",
        "abseil-cpp-src", "aurora_nod-src", "freetype-src", "png-src", "zlib-src", "xxhash-src",
    ).forEach { dep ->
        from(nativeStagingDir) {
            include("**/_deps/$dep/LICENSE*")
            include("**/_deps/$dep/COPYING*")
            eachFile { relativePath = RelativePath(true, "licenses", "$dep.txt") }
            includeEmptyDirs = false
        }
    }
}

tasks.named("preBuild") {
    dependsOn(syncSdlJava, checkSdlVersion, syncPortAssets, syncCannonTextures, syncLauncherRes)
}
// The fetched packages' notices exist only after the native build has fetched them.
tasks.named("syncLicenseNotices") {
    mustRunAfter(tasks.matching { it.name.startsWith("buildCMake") || it.name.startsWith("externalNativeBuild") })
}
tasks.matching { it.name.matches(Regex("""merge\w*Assets""")) || it.name.lowercase().contains("lint") }.configureEach {
    dependsOn(syncLicenseNotices)
}

// The launcher panel (PrimedGun's Quest launcher widgets); the game activity uses
// none of these.
dependencies {
    implementation("androidx.core:core-ktx:1.17.0")
    implementation("androidx.appcompat:appcompat:1.7.1")
    implementation("androidx.fragment:fragment-ktx:1.8.9")
    implementation("androidx.recyclerview:recyclerview:1.4.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.4")
    implementation("com.google.android.material:material:1.13.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")
}
