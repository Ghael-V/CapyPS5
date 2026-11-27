// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "org.capyps5.emulator"
    compileSdk = 35

    defaultConfig {
        applicationId = "org.capyps5.emulator"
        minSdk = 31
        targetSdk = 34
        versionCode = 8
        versionName = "0.2.0"

        ndk {
            abiFilters.add("arm64-v8a")
        }

        // Where "Recommended settings" asks for suggestions: the same server as Lemon's (it holds
        // the AI key; the app never does). A test build can point it at a local mock with
        // -PaiConfigUrl=http://...; only then is cleartext HTTP allowed.
        val aiConfigUrl = (project.findProperty("aiConfigUrl") as String?) ?: "https://ai.lemon-emu.org/v1"
        buildConfigField("String", "AI_CONFIG_URL", "\"$aiConfigUrl\"")
        manifestPlaceholders["usesCleartextTraffic"] = aiConfigUrl.startsWith("http://").toString()
    }

    lint {
        // MainActivity and GameActivity extend the abstract CapyActivity (a ComponentActivity);
        // lint does not follow it and reports them as not being activities.
        disable += "Instantiatable"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
        debug {
            isDebuggable = true
            applicationIdSuffix = ".debug"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    sourceSets {
        getByName("main") {
            jniLibs.srcDirs("src/main/jniLibs")
        }
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2024.12.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.documentfile:documentfile:1.0.1")

    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
