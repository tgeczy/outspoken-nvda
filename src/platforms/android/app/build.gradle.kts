import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// The licence notices the Setup page shows and the APK carries: outSPOKEN's
// own, Musashi's, the emoji tables', Unicode's, Kotlin's.  Staged from the
// folder beside this build, so the APK never ships without them.
val nativeNotices = layout.buildDirectory.dir("generated/outspokenLicenses")
val stageNativeNotices = tasks.register<Sync>("stageNativeNotices") {
    from(rootProject.file("licenses"))
    into(nativeNotices)
}
// The native library is prebuilt by build_android.sh at the repository root
// -- the same host sources the NVDA DLL and the SAPI program come from, cross
// built with the NDK -- and dropped under jniLibs.  Gradle only checks it is
// there for every ABI it packages.
val verifyNativeBuild = tasks.register("verifyNativeBuild") {
    doLast {
        for (abi in listOf("arm64-v8a", "armeabi-v7a")) {
            val library = file("src/main/jniLibs/$abi/liboutspoken.so")
            check(library.isFile) { "Build the $abi library with `sh build_android.sh` first" }
        }
    }
}
tasks.matching { it.name == "preBuild" }.configureEach {
    dependsOn(stageNativeNotices, verifyNativeBuild)
}

android {
    namespace = "com.outspoken.tts"
    compileSdk = 35
    buildFeatures { aidl = true }

    defaultConfig {
        // Permanent once shipped: Android treats a different id as a
        // different app.
        applicationId = "com.outspoken.tts"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "2.0.0"

        ndk {
            abiFilters += listOf("armeabi-v7a", "arm64-v8a")
        }
    }

    // Kotlin sources live under src/main/kotlin, and the desktop-JVM tests
    // of the pure pieces -- the update check, the move into protected
    // storage, the zip importer -- under src/test/kotlin.
    sourceSets["main"].java.srcDirs("src/main/kotlin")
    sourceSets["test"].java.srcDirs("src/test/kotlin")
    sourceSets["main"].assets.srcDir(nativeNotices)

    // Release signing.  Android refuses to install an unsigned APK, so a
    // release is only a release once it is signed with the project's key.
    // The key lives outside the repository: a `signing.properties` beside
    // settings.gradle.kts, ignored by Git, naming it -- the same file, with
    // the same four names, that Panthera's and TGSpeechBox's Android builds
    // read, so one convention serves all of the maintainer's apps:
    //
    //   STORE_FILE=C:/Users/you/release.keystore
    //   STORE_PASSWORD=...
    //   KEY_ALIAS=...
    //   KEY_PASSWORD=...
    //
    // Without that file the release build still succeeds and stays unsigned,
    // which is what CI and anyone without the key should get.  The same key
    // must sign every future release, or Android treats the update as a
    // different app and refuses it: keep it backed up.
    val signingProperties = rootProject.file("signing.properties")
    if (signingProperties.isFile) {
        val keys = Properties().apply { signingProperties.inputStream().use { load(it) } }
        signingConfigs {
            create("release") {
                storeFile = file(keys.getProperty("STORE_FILE"))
                storePassword = keys.getProperty("STORE_PASSWORD")
                keyAlias = keys.getProperty("KEY_ALIAS")
                keyPassword = keys.getProperty("KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            if (signingProperties.isFile) signingConfig = signingConfigs.getByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    // Desktop-JVM tests only. The app itself depends on nothing but the
    // platform; org.json is the real library here because android.jar's copy
    // is a stub that throws.
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")
}
