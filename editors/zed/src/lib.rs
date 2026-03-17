use zed_extension_api as zed;

struct XsExtension;

impl zed::Extension for XsExtension {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        _id: &zed::LanguageServerId,
        _worktree: &zed::Worktree,
    ) -> zed::Result<zed::Command> {
        Ok(zed::Command {
            command: "xs".to_string(),
            args: vec!["lsp".to_string()],
            env: vec![],
        })
    }
}

zed::register_extension!(XsExtension);
