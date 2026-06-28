import { existsSync, mkdirSync } from "node:fs";
import { getArch, getBinExt, getOS, isNewer } from "./os";
import { join, resolve } from "node:path"
import download from "./download";
import { decompress } from "./compress";
import { homedir } from "node:os";
import { $ } from "bun";
import { config } from "dotenv"

config();

export async function isZig() {
    try {
        const version = await Bun.$`zig version`.quiet().text();
        return version.trim() === "0.13.0";
    } catch {
        return false;
    }
}

/**
 * Adds a specific directory to the user's permanent system PATH.
 * @param targetDir The absolute path to the directory containing the 'zig' executable.
 */
async function addZigToPath(targetDir: string) {
    // Ensure the directory actually exists before adding it to PATH
    if (!existsSync(targetDir)) {
        throw new Error(`The specified directory does not exist: ${targetDir}`);
    }

    const OS = getOS(); // "windows", "linux", or "darwin"

    console.log(`Configuring PATH for platform: ${OS}...`);

    if (OS === "win") {
        try {
            const currentPath = process.env.PATH || "";

            if (currentPath.includes(targetDir)) {
                console.log("Zig directory is already present in your Windows PATH.");
                return;
            }

            await $`setx PATH "%PATH%;${targetDir};${currentPath}"`;
            console.log("Successfully updated Windows PATH Registry.");
            console.log("Restart your terminal or IDE for the 'zig' command to activate.");
        } catch (err) {
            console.error("Failed to update Windows registry path:", err);
        }
    } else {
        const isZsh = process.env.SHELL?.includes("zsh");
        const shellFile = isZsh
            ? join(homedir(), ".zshrc")
            : join(homedir(), ".bashrc");

        if (!existsSync(shellFile)) {
            console.log(`Creating missing shell profile layout at: ${shellFile}`);
            await Bun.write(shellFile, "");
        }

        const currentContent = await Bun.file(shellFile).text();

        if (currentContent.includes(targetDir)) {
            console.log(`Zig directory already configured in ${shellFile}`);
            return;
        }

        const exportLine = `\n# Zig Compiler Toolchain\nexport PATH="$PATH:${targetDir}"\n`;
        await Bun.write(shellFile, currentContent + exportLine);

        console.log(`Successfully appended path strings to ${shellFile}`);
        console.log(`To use 'zig' right away, run: source ${shellFile}`);
    }
}

/**
 * 
 * @param path 
 * the path of workspace
 */
export async function installZig(path: string) {
    const zigVersion = "0.13.0";
    const OS = getOS();
    const arch = getArch();
    const zigArch = arch === "arm64" ? "aarch64" : "x86_64";

    const zigName = `zig-${OS === "win" ? "windows" : OS}-${zigArch}-${zigVersion}`
    const zigDownloadedExt = `.${OS === "win" ? "zip" : "tar.xz"}`
    const zigDownloadedFilename = zigName + zigDownloadedExt;
    const downloadUrl = `https://ziglang.org/download/0.13.0/${zigDownloadedFilename}`;
    const zigFolder = join(path, "vendors", "zig");

    mkdirSync(zigFolder, { recursive: true });
    if (!existsSync(join(zigFolder, zigDownloadedFilename)))
        await download(downloadUrl, join(zigFolder, zigDownloadedFilename));

    if (!existsSync(join(zigFolder, zigDownloadedFilename))) throw new Error(`Error on download ${zigName}`);

    await decompress(join(zigFolder, zigDownloadedFilename), join(path, "vendors", "zig-temp"));
    if (existsSync(join(path, "vendors", "zig-temp", `${zigName}.tar`))) {
        await decompress(join(path, "vendors", "zig-temp", `${zigName}.tar`), join(join(path, "vendors", "zig-temp")))
    }

    let zigTempPath = join(path, "vendors", "zig-temp", zigName);
    const zigExt = OS === "win" ? ".exe" : "";
    const zigFileName = "zig" + zigExt;
    if (existsSync(join(zigTempPath, zigName))) {
        zigTempPath = join(zigTempPath, zigName);
    }
    await addZigToPath(join(zigTempPath, zigFileName));
}

/**
 * 
 * @param path 
 * workspace path
 * @param args
 */
export async function runZigbuild(path: string, buildName: string, args: string[]) {
    path = resolve(path);
    const checkZig = await isZig();
    if (!checkZig) {
        await installZig(path)
    }
    const sourceDir = join(path, "src", buildName);
    if (!existsSync(sourceDir)) {
        throw new Error(`${buildName} source dir not found at ${sourceDir}`);
    }
    const Source = join(sourceDir, "main.zig");
    const buildSource = join(sourceDir, "build.zig");
    const outSource = join(sourceDir, "zig-out", "bin", `${buildName}${getBinExt()}`);

    if (!existsSync(Source)) {
        throw new Error(`main.zig not found at ${Source}`);
    }

    console.log("\n" + Array(10).fill("=").join("") + ` Build ${buildName} ` + Array(10).fill("=").join(""))
    if (isNewer(Source, [outSource])) {
        console.log(`${buildName} is changed. Building...`);
        await $`rm -rf .zig-cache`.cwd(sourceDir);
        await $`zig build ${args}`.cwd(sourceDir);
        console.log(`${buildName} build done.`)
    }
    else {
        console.log(`${buildName} is not changed. Skipping...`)
    }
}