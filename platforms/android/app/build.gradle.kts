plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.fegno.livewall"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.fegno.livewall"
        // Android 10. Below this there is no thermal-status API, which is one of
        // the gates the whole design is built around, and MediaCodec's
        // surface-to-surface transcode path is materially less reliable.
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "1.0.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlin {
        compilerOptions {
            jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
        }
    }

    sourceSets {
        getByName("main") {
            kotlin.srcDirs("src/main/kotlin")
        }
        getByName("test") {
            kotlin.srcDirs("src/test/kotlin")
        }
        getByName("androidTest") {
            kotlin.srcDirs("src/androidTest/kotlin")
            // The repository's own sample loops, not a copy of them. The
            // transcode test needs a real video and these are the only ones this
            // project ships.
            assets.srcDirs("../../../Samples")
        }
    }

    // No AndroidX, no Compose, no Material, no media3. Everything the app needs
    // — MediaCodec, MediaMuxer, EGL, WallpaperService — ships in the platform,
    // and the runtime numbers this app exists to defend are easier to keep when
    // there is nothing else in the process.
    buildFeatures {
        buildConfig = false
        resValues = false
        shaders = false
        aidl = false
        renderScript = false
    }

    packaging {
        resources.excludes += setOf("META-INF/*.kotlin_module", "kotlin/**", "DebugProbesKt.bin")
    }

    testOptions {
        unitTests.isReturnDefaultValues = false
    }
}

dependencies {
    // The unit suite is pure logic — frame pacing, output sizing, fit geometry,
    // index decoding — so it needs no device, no Robolectric and no android.jar
    // stubs.
    testImplementation(libs.junit)

    // The transcode path is the one thing that cannot be tested that way: it is
    // an encoder, a muxer and an EGL context, and all three are the device's.
    // Test-only — nothing here reaches the shipped APK.
    androidTestImplementation(libs.junit)
    androidTestImplementation(libs.androidx.test.runner)
}
