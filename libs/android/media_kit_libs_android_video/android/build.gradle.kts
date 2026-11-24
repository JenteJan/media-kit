import java.io.File
import java.net.URI
import java.security.MessageDigest

group = "com.alexmercerind.media_kit_libs_android_video"
version = "1.0"

buildscript {
    val kotlinVersion = "2.4.20"
    repositories {
        google()
        mavenCentral()
    }

    dependencies {
        classpath("com.android.tools.build:gradle:9.3.1")
        classpath("org.jetbrains.kotlin:kotlin-gradle-plugin:$kotlinVersion")
    }
}

allprojects {
    repositories {
        google()
        mavenCentral()
    }
}

plugins {
    id("com.android.library")
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

android {
    namespace = "com.alexmercerind.media_kit_libs_android_video"
    compileSdk = flutter.compileSdkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets {
        getByName("main") {
            java.srcDirs("src/main/kotlin")
        }
    }

    defaultConfig {
        minSdk = 24
    }
}

dependencies {
    implementation(fileTree(layout.buildDirectory.dir("output")) { include("*.jar") })
}

fun File.md5(): String {
    val digest = MessageDigest.getInstance("MD5")
    inputStream().use { input ->
        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            digest.update(buffer, 0, count)
        }
    }
    return digest.digest().joinToString("") { byte -> "%02x".format(byte.toInt() and 0xff) }
}

val downloadDependencies by tasks.registering {
    doLast {
        val outputDirectory = layout.buildDirectory.dir("output").get().asFile
        project.delete(outputDirectory)
        outputDirectory.mkdirs()

        val filesToDownload = listOf(
            Triple(
                "https://github.com/media-kit/libmpv-android-video-build/releases/download/v1.1.11/full-arm64-v8a.jar",
                "86f2c8faeb66af1878b3a16f67831cb3",
                layout.buildDirectory.file("v1.1.11/full-arm64-v8a.jar").get().asFile,
            ),
            Triple(
                "https://github.com/media-kit/libmpv-android-video-build/releases/download/v1.1.11/full-armeabi-v7a.jar",
                "93542d40e44f3afad3aa773674e8eaa5",
                layout.buildDirectory.file("v1.1.11/full-armeabi-v7a.jar").get().asFile,
            ),
            Triple(
                "https://github.com/media-kit/libmpv-android-video-build/releases/download/v1.1.11/full-x86_64.jar",
                "44b7efdbaf3626d6b24afaf5d497a369",
                layout.buildDirectory.file("v1.1.11/full-x86_64.jar").get().asFile,
            ),
            Triple(
                "https://github.com/media-kit/libmpv-android-video-build/releases/download/v1.1.11/full-x86.jar",
                "e69a9bbd7fb587deb33dd10fecc2fecc",
                layout.buildDirectory.file("v1.1.11/full-x86.jar").get().asFile,
            ),
        )

        filesToDownload.forEach { (url, expectedMd5, destination) ->
            if (destination.exists() && destination.md5() != expectedMd5) {
                destination.delete()
                println("MD5 mismatch. File deleted: $destination")
            }

            if (!destination.exists()) {
                destination.parentFile.mkdirs()
                println("Downloading file from: $url")
                URI.create(url).toURL().openStream().use { input ->
                    destination.outputStream().use(input::copyTo)
                }
                if (destination.md5() != expectedMd5) {
                    throw GradleException("MD5 verification failed for $destination")
                }
            }

            project.copy {
                from(destination)
                into(outputDirectory)
            }
        }
    }
}

tasks.named("preBuild").configure {
    dependsOn(downloadDependencies)
}
