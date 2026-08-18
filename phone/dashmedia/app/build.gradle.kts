plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.kao.dashmedia"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.kao.dashmedia"
        minSdk = 30
        // Deliberately pinned to the XP3900's own API level. This app is
        // sideloaded and will never see Play, so matching the device exactly
        // is worth more than a modern targetSdk: it keeps the legacy
        // permission model (no BLUETOOTH_CONNECT, no POST_NOTIFICATIONS) and
        // the pre-34 foreground-service rules, none of which the phone has.
        //noinspection ExpiredTargetSdkVersion,OldTargetApi
        targetSdk = 30
        versionCode = 1
        versionName = "0.1"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
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
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.recyclerview)
    implementation(libs.media3.exoplayer)
    implementation(libs.media3.session)
    testImplementation(libs.junit)
}
