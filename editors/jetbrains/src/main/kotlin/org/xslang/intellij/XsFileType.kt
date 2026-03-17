package org.xslang.intellij

import com.intellij.openapi.fileTypes.LanguageFileType
import javax.swing.Icon

class XsFileType : LanguageFileType(XsLanguage) {
    override fun getName(): String = "XS"
    override fun getDescription(): String = "XS source file"
    override fun getDefaultExtension(): String = "xs"
    override fun getIcon(): Icon? = null

    companion object {
        @JvmField val INSTANCE = XsFileType()
    }
}
