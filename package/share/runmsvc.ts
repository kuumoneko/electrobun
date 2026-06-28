import { getOS } from "./os";
import { join } from "node:path"
import { existsSync } from "node:fs";
import { $ } from "bun"

async function findMsvcTools(): Promise<string> {
    if (getOS() !== "win") throw new Error("Msvc only runs on Windows");

    try {
        const vswherePath = join(
            process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)",
            "Microsoft Visual Studio", "Installer", "vswhere.exe",
        );
        if (!existsSync(vswherePath)) return "";

        const vsInstallResult = await $`powershell -command "& '${vswherePath}' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`.quiet();
        if (vsInstallResult.exitCode !== 0 || !vsInstallResult.stdout.toString().trim()) return "";

        const vsInstallPath = vsInstallResult.stdout.toString().trim();

        const VCVARSALL_PATH = join(vsInstallPath, "VC", "Auxiliary", "Build", "vcvarsall.bat");
        return existsSync(VCVARSALL_PATH) ? VCVARSALL_PATH : "";
    } catch {
        return "";
    }
}

export default async function runMsvcCommand(command: string) {
    const VCVARSALL_PATH = await findMsvcTools();
    if (VCVARSALL_PATH.length < 1) {
        console.log("Could not locate MSVC tools, using default tool names");
        return await $`${command}`;
    }
    try {
        const result = await $`cmd /c ""${VCVARSALL_PATH}" x64 >nul && ${command}"`;
        return result;
    } catch (error) {
        throw error;
    }
}