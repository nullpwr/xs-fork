package org.xslang.intellij

import com.intellij.openapi.project.Project
import com.intellij.platform.lsp.api.LspServerSupportProvider
import com.intellij.platform.lsp.api.ProjectWideLspServerDescriptor
import com.intellij.openapi.vfs.VirtualFile

class XsLspServerSupportProvider : LspServerSupportProvider {
    override fun fileOpened(
        project: Project,
        file: VirtualFile,
        serverStarter: LspServerSupportProvider.LspServerStarter,
    ) {
        if (file.extension == "xs") {
            serverStarter.ensureServerStarted(XsLspServerDescriptor(project))
        }
    }
}

private class XsLspServerDescriptor(project: Project) :
    ProjectWideLspServerDescriptor(project, "XS") {

    override fun isSupportedFile(file: VirtualFile) = file.extension == "xs"

    override fun createCommandLine() =
        com.intellij.execution.configurations.GeneralCommandLine("xs", "lsp")
}
