import { join, dirname } from "node:path";

async function getRceditPath(): Promise<string> {
    const rceditPath = join(dirname(process.execPath), "rcedit-x64.exe");
    if (!(await Bun.file(rceditPath).exists())) {
        throw new Error(
            `rcedit binary not found at ${rceditPath}. ` +
            `Make sure 'rcedit' is installed in your project's node_modules.`,
        );
    }
    return rceditPath;
}

export async function embedIcon(exePath: string, iconPath: string): Promise<void> {
    const rceditPath = await getRceditPath();
    if (!(await Bun.file(iconPath).exists())) {
        throw new Error(`Icon path not found at ${iconPath}`);
    }
    await Bun.spawn([rceditPath, exePath, "--set-icon", iconPath], {
        stdout: "inherit",
        stderr: "inherit",
    }).exited;
}

export interface MetadataOptions {
    name: string;
    identifier: string;
    version: string;
}

export async function embedMetadata(exePath: string, options: MetadataOptions): Promise<void> {
    const rceditPath = await getRceditPath();
    await Bun.spawn([
        rceditPath, exePath,
        "--set-version-string", "ProductName", options.name,
        "--set-version-string", "FileDescription", options.name,
        "--set-version-string", "CompanyName", options.identifier,
        "--set-version-string", "ApplicationCompany", options.identifier,
        "--set-version-string", "InternalFilename", options.name,
        "--set-file-version", options.version,
        "--set-product-version", options.version,
        "--set-version-string", "LegalCopyright", "©2026 Electrobun. All rights reserved.",
    ], { stdout: "inherit", stderr: "inherit" }).exited;
}
