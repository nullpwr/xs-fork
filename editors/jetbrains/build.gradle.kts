plugins {
    id("java")
    id("org.jetbrains.kotlin.jvm") version "1.9.22"
    id("org.jetbrains.intellij") version "1.17.3"
}

group = "org.xslang"
version = "0.1.0"

repositories {
    mavenCentral()
}

intellij {
    version.set("2023.3.6")
    type.set("IU") // Ultimate; LSP API requires it
    plugins.set(listOf("textmate"))
}

tasks {
    withType<JavaCompile> {
        sourceCompatibility = "17"
        targetCompatibility = "17"
    }
    withType<org.jetbrains.kotlin.gradle.tasks.KotlinCompile> {
        kotlinOptions.jvmTarget = "17"
    }
    patchPluginXml {
        sinceBuild.set("233")
        untilBuild.set("251.*")
    }
}
