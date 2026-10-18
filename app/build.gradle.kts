plugins {
    id("com.android.application")
}

android {
    namespace = "io.github.krhomv.widscan58"
    compileSdk = 34

    defaultConfig {
        applicationId = "io.github.krhomv.widscan58"
        minSdk = 26 // UsbRequest.queue(ByteBuffer)
        targetSdk = 34
        versionCode = (project.findProperty("versionCode") as String?)?.toInt() ?: 1
        versionName = (project.findProperty("versionName") as String?) ?: "dev"
    }

    signingConfigs {
        // A fixed key (committed, not secret) so every build installs over the last.
        getByName("debug") {
            storeFile = file("debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    androidResources {
        // Keep the web UI's files (and the "_"-less names) exactly as built.
        ignoreAssetsPattern = "!.svn:!.git:!.ds_store:!*.scc:!CVS:!thumbs.db:!picasa.ini:!*~"
    }
}
