import com.android.build.gradle.internal.api.ApkVariantOutputImpl
import com.launchers_plugin.renderer.buildscript.RendererConfig
import com.launchers_plugin.renderer.buildscript.buildEnvs
import com.launchers_plugin.renderer.buildscript.buildJsonValue
import com.launchers_plugin.renderer.buildscript.legacyManifest
import com.launchers_plugin.renderer.buildscript.nativePath
import com.launchers_plugin.renderer.buildscript.renderer

buildscript {
    repositories {
        maven("https://jitpack.io")
    }
    dependencies {
        classpath("com.github.ZalithLauncher.RendererPlugin-v2:dsl:1.0-alpha6")
    }
}

plugins {
    id("com.android.application")
}

apply(plugin = "com.launchers_plugin.renderer.dsl")

fun Project.mobileGlAbiFilters(): List<String> {
    val abiList = (findProperty("mobilegl.abis") ?: System.getenv("MOBILEGL_ABIS") ?: "arm64-v8a").toString()
    return if (abiList.equals("all", ignoreCase = true)) {
        listOf("arm64-v8a", "x86_64")
    } else {
        abiList.split(',').map(String::trim).filter(String::isNotEmpty)
    }
}

fun Project.mobileGlCmakeCompilerLauncher(): String =
    (findProperty("mobilegl.cmakeCompilerLauncher") ?: System.getenv("MOBILEGL_CMAKE_COMPILER_LAUNCHER") ?: "").toString().trim()

fun Project.runGit(vararg arguments: String): String? = runCatching {
    ProcessBuilder("git", *arguments)
        .directory(rootDir)
        .redirectErrorStream(true)
        .start()
        .let { process ->
            val output = process.inputStream.bufferedReader().use { it.readText().trim() }
            output.takeIf { process.waitFor() == 0 && it.isNotEmpty() }
        }
}.getOrNull()

val signingStoreFile = file("../keystore.jks")
val signingStorePassword = System.getenv("SIGNING_STORE_PASSWORD")
val signingKeyAlias = System.getenv("SIGNING_KEY_ALIAS")
val signingKeyPassword = System.getenv("SIGNING_KEY_PASSWORD")
val releaseSigningReady = signingStoreFile.exists()
    && !signingStorePassword.isNullOrEmpty()
    && !signingKeyAlias.isNullOrEmpty()
    && !signingKeyPassword.isNullOrEmpty()
val debuggableRelease = (findProperty("mobilegl.debuggableRelease") ?: "false").toString().toBoolean()

val mobileGlVersionMajor = 26
val mobileGlVersionMinor = 7
val mobileGlGitShortHash = runGit("rev-parse", "--short=7", "HEAD") ?: "nogit"
val mobileGlMonthlyRevision = runGit(
    "rev-list",
    "--count",
    "--since=${String.format("%d-%02d-01T00:00:00", 2000 + mobileGlVersionMajor, mobileGlVersionMinor)}",
    "HEAD",
)?.toIntOrNull() ?: 0
val mobileGlApkSuffix = (findProperty("mobilegl.apkSuffix") ?: System.getenv("MOBILEGL_APK_SUFFIX") ?: mobileGlGitShortHash)
    .toString()
    .ifBlank { "nogit" }

val pluginRendererConfig = buildJsonValue {
    renderer(
        displayName = "MobileGL",
        rendererId = "opengles3",
        rendererGLPath = nativePath("libMobileGL.so"),
        rendererEGLPath = nativePath("libMobileGL.so"),
        dlopenLibPaths = emptyList(),
        env = buildEnvs {
            normal("LIBGL_ES", "3")
            selectable(
                key = "MOBILEGL_BACKEND_TYPE",
                title = RendererConfig.MetaString("mobilegl_backend_type_title"),
                items = RendererConfig.EnvItems("DirectGLES", listOf("DirectVulkan")),
            )
            toggleable("MOBILEGL_DISABLE_TIMERQUERY", "1", false, RendererConfig.MetaString("mobilegl_disable_timerquery_title"))
            toggleable("MOBILEGL_DISABLE_SUBGROUP", "1", false, RendererConfig.MetaString("mobilegl_disable_subgroup_title"))
            toggleable("MOBILEGL_MAGMA_R11G11B10F_FALLBACK", "1", false, RendererConfig.MetaString("mobilegl_magma_r11g11b10f_fallback_title"))
            customizable("MOBILEGL_MAGMA_FRAMESINFLIGHT", "3", RendererConfig.MetaString("mobilegl_magma_frames_inflight_title"))
            toggleable("MOBILEGL_AVOID_SAMPLER_MIPMAP_MIN_FILTER", "1", false, RendererConfig.MetaString("mobilegl_avoid_sampler_mipmap_min_filter_title"))
            toggleable("MOBILEGL_USE_ANGLE", "1", false, RendererConfig.MetaString("mobilegl_use_angle_title"))
        },
        minMCVer = null,
        maxMCVer = null,
    )
}

android {
    namespace = "top.mobilegl.plugin"
    compileSdk = 34
    ndkVersion = "27.3.13750724"

    defaultConfig {
        applicationId = "top.mobilegl.plugin"
        minSdk = 26
        targetSdk = 34
        versionCode = mobileGlVersionMajor * 1_000_000 + mobileGlVersionMinor * 10_000 + mobileGlMonthlyRevision
        versionName = "%d.%02d.%s".format(mobileGlVersionMajor, mobileGlVersionMinor, mobileGlGitShortHash)
        resValue("string", "config", pluginRendererConfig)

        manifestPlaceholders.putAll(legacyManifest {
            displayName = "MobileGL"
            rendererName = "MobileGL"
            rendererLib = "libMobileGL.so"
            eglLib = "/libMobileGL.so"
            minMCVer = ""
            maxMCVer = ""
            boatEnv {
                put("LIBGL_ES", "3")
                put("POJAV_RENDERER", "opengles3")
                put("MOBILEGL_BACKEND_TYPE", "DirectGLES")
            }
            pojavEnv {
                put("LIBGL_ES", "3")
                put("POJAV_RENDERER", "opengles3")
                put("MOBILEGL_BACKEND_TYPE", "DirectGLES")
            }
        })
        manifestPlaceholders["appLabel"] = "MobileGL"

        ndk {
            abiFilters += mobileGlAbiFilters()
        }
        externalNativeBuild {
            cmake {
                mobileGlCmakeCompilerLauncher().takeIf(String::isNotEmpty)?.let { compilerLauncher ->
                    arguments += listOf(
                        "-DCMAKE_C_COMPILER_LAUNCHER=$compilerLauncher",
                        "-DCMAKE_CXX_COMPILER_LAUNCHER=$compilerLauncher",
                    )
                }
            }
        }
    }

    buildFeatures {
        resValues = true
    }

    flavorDimensions += "profile"
    productFlavors {
        create("plugin") {
            dimension = "profile"
        }
        create("trace") {
            dimension = "profile"
            applicationIdSuffix = ".trace"
            versionNameSuffix = "-trace"
        }
    }

    if (releaseSigningReady) {
        signingConfigs {
            create("release") {
                storeFile = signingStoreFile
                storePassword = signingStorePassword
                keyAlias = signingKeyAlias
                keyPassword = signingKeyPassword
            }
        }
    }

    buildTypes {
        getByName("release") {
            isDebuggable = debuggableRelease
            isMinifyEnabled = false
            if (releaseSigningReady) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/trace/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
            excludes += "**/libSPIRV-Tools-shared.so"
        }
    }
}

android.applicationVariants.configureEach {
    outputs.configureEach {
        (this as ApkVariantOutputImpl).outputFileName = when (flavorName) {
            "plugin" -> "MobileGL-plugin-release-$mobileGlApkSuffix.apk"
            "trace" -> "MobileGL-plugin-trace-release-$mobileGlApkSuffix.apk"
            else -> outputFileName
        }
    }
}

androidComponents {
    onVariants(selector().withFlavor("profile" to "plugin")) { variant ->
        variant.packaging.jniLibs.excludes.add("**/libtrace_replay_runner.so")
    }
}

dependencies {
    implementation(project(":MobileGL"))
}
