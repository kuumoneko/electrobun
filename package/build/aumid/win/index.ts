import { $ } from 'bun';
import { existsSync } from 'fs';
import { join } from 'path';
import os from "node:os"
let VCVARSALL_PATH;
async function findMsvcTools() {
    if (os.platform() !== "win32") return;

    try {
        const vswherePath = join(
            process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)",
            "Microsoft Visual Studio", "Installer", "vswhere.exe",
        );
        if (!existsSync(vswherePath)) {
            console.log("vswhere not found, using default tool names");
            return;
        }
        const vsInstallResult = await $`powershell -command "& '${vswherePath}' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath"`.quiet();
        if (vsInstallResult.exitCode !== 0 || !vsInstallResult.stdout.toString().trim()) {
            console.log("Could not find Visual Studio installation path");
            return;
        }
        const vsInstallPath = vsInstallResult.stdout.toString().trim();
        VCVARSALL_PATH = join(vsInstallPath, "VC", "Auxiliary", "Build", "vcvarsall.bat");
        if (!existsSync(VCVARSALL_PATH)) {
            console.log("vcvarsall.bat not found at expected location");
            VCVARSALL_PATH = "";
            return;
        }
        console.log("✓ Found MSVC tools with vcvarsall.bat");
    } catch {
        console.log("Could not locate MSVC tools, using default tool names");
    }
}

export default async function build_win_aumid() {
    try {
        const cl = await $`C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe`;
        // const link = await $`link`;

        console.log(cl)
        // console.log(link)
    }
    catch (e) {
        console.log(e)
        throw new Error("cl or link not found in this system");
    }
}

findMsvcTools();