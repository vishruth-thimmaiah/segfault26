const vscode = require('vscode');
const path = require('path');
const fs = require('fs');

function activate(context) {
    const factory = {
        createDebugAdapterDescriptor(session) {
            if (session.configuration && session.configuration.debugServer) {
                return new vscode.DebugAdapterServer(session.configuration.debugServer);
            }
            let ocldbgPath = 'ocldbg';
            if (vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders.length > 0) {
                const wsRoot = vscode.workspace.workspaceFolders[0].uri.fsPath;
                const candidate = path.join(wsRoot, 'build', 'ocldbg');
                if (fs.existsSync(candidate)) {
                    ocldbgPath = candidate;
                }
            }
            return new vscode.DebugAdapterExecutable(ocldbgPath, ['--dap']);
        }
    };
    context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory('ocldbg', factory));
}

function deactivate() {}

module.exports = {
    activate,
    deactivate
};
