package org.xslang.intellij

import org.jetbrains.plugins.textmate.api.TextMateBundleProvider
import java.io.File

class XsTextMateBundleProvider : TextMateBundleProvider {
    override fun getBundles(): List<TextMateBundleProvider.PluginBundle> {
        // Resolves to the textmate/ directory inside the plugin's classpath.
        val resourceUrl = javaClass.classLoader.getResource("textmate")
            ?: return emptyList()
        return listOf(
            TextMateBundleProvider.PluginBundle(
                "XS",
                File(resourceUrl.toURI()).toPath()
            )
        )
    }
}
