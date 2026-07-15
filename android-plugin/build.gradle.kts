import com.android.build.gradle.LibraryExtension

plugins {
    id("com.android.application") version "8.6.0" apply false
    id("com.android.library") version "8.6.0" apply false
}

fun Project.mobileGlAbiFilters(): List<String> {
    val abiList = (findProperty("mobilegl.abis") ?: System.getenv("MOBILEGL_ABIS") ?: "arm64-v8a").toString()
    return if (abiList.equals("all", ignoreCase = true)) {
        listOf("arm64-v8a", "x86_64")
    } else {
        abiList.split(',').map(String::trim).filter(String::isNotEmpty)
    }
}

fun Project.mobileGlLogActiveLevel(): String =
    (findProperty("mobilegl.logLevel") ?: System.getenv("MOBILEGL_LOG_ACTIVE_LEVEL") ?: "MOBILEGL_LOG_LEVEL_INFO").toString()

fun Project.mobileGlCmakeCompilerLauncher(): String =
    (findProperty("mobilegl.cmakeCompilerLauncher") ?: System.getenv("MOBILEGL_CMAKE_COMPILER_LAUNCHER") ?: "").toString().trim()

subprojects {
    plugins.withId("com.android.library") {
        extensions.configure<LibraryExtension> {
            defaultConfig {
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
                        cppFlags += "-DMOBILEGL_LOG_ACTIVE_LEVEL=${mobileGlLogActiveLevel()}"
                    }
                }
            }
        }
    }
}
