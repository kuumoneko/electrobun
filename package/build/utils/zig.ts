// import { $ } from "bun";
// import { os } from "bun";
// import { mkdirSync, existsSync } from "node:fs";
// import { join } from "node:path";

import { existsSync, mkdirSync } from "node:fs";
import { getArch, getOS } from "./os";
import { join } from "node:path"
import download from "./download";
import { decompress } from "./compress";
import { homedir } from "node:os";
import { $ } from "bun";

import { config } from "dotenv"
config()
// // Define the target Zig version and path structures
// const ZIG_VERSION = "0.13.0";
// const INSTALL_DIR = join(os.homedir(), ".zig-toolchain");

// interface PlatformConfig {
//     url: string;
//     archiveName: string;
//     isZip: boolean;
// }

// function getPlatformConfig(): PlatformConfig {
//     const platform = os.platform(); // "windows", "linux", "darwin" (macOS)
//     const arch = os.arch();         // "x64", "arm64"

//     let zigArch = arch === "x64" ? "x86_64" : arch;
//     let zigOS = platform === "darwin" ? "macos" : platform;
//     let ext = platform === "windows" ? "zip" : "tar.xz";

//     const archiveName = `zig-${zigOS}-${zigArch}-${ZIG_VERSION}`;
//     const url = `https://ziglang.org/download/${ZIG_VERSION}/${archiveName}.${ext}`;

//     return { url, archiveName, isZip: platform === "windows" };
// }

// async function updateSystemPath(targetPath: string) {
//     const platform = os.platform();

//     if (platform === "windows") {
//         console.log("Setting environment variable via registry...");
//         // setx appends permanently to the Windows User PATH registry key
//         await $`setx PATH "%PATH%;${targetPath}"`;
//         console.log("✅ PATH update applied. Open a new terminal to use 'zig'.");
//     } else {
//         // Linux and macOS
//         const shellFile = process.env.SHELL?.includes("zsh")
//             ? join(os.homedir(), ".zshrc")
//             : join(os.homedir(), ".bashrc");

//         if (existsSync(shellFile)) {
//             const exportLine = `\nexport PATH="$PATH:${targetPath}"\n`;
//             const currentContent = await Bun.file(shellFile).text();

//             if (!currentContent.includes(targetPath)) {
//                 await Bun.write(shellFile, currentContent + exportLine);
//                 console.log(`✅ Appended path to ${shellFile}`);
//                 console.log(`👉 Run 'source ${shellFile}' or open a new shell to start using 'zig'.`);
//             } else {
//                 console.log("ℹ️ Zig path already detected in shell configuration.");
//             }
//         } else {
//             console.error(`❌ Could not locate profile file at: ${shellFile}. Add manually: ${targetPath}`);
//         }
//     }
// }

// async function run() {
//     const config = getPlatformConfig();
//     const downloadPath = join(INSTALL_DIR, `zig-archive.${config.isZip ? 'zip' : 'tar.xz'}`);
//     const finalExecutableDir = join(INSTALL_DIR, config.archiveName);

//     // 1. Double check if it's already installed there
//     if (existsSync(join(finalExecutableDir, os.platform() === "windows" ? "zig.exe" : "zig"))) {
//         console.log(`⚡ Zig ${ZIG_VERSION} is already installed at ${finalExecutableDir}`);
//         await updateSystemPath(finalExecutableDir);
//         return;
//     }

//     // 2. Prepare workspace
//     if (!existsSync(INSTALL_DIR)) mkdirSync(INSTALL_DIR, { recursive: true });

//     console.log(`🌐 Fetching Zig source: ${config.url}`);

//     // 3. Stream data to file via Bun
//     const response = await fetch(config.url);
//     if (!response.ok) throw new Error(`Download failed with status: ${response.status}`);
//     await Bun.write(downloadPath, response);
//     console.log("📥 Download complete.");

//     // 4. Extract archive cross-platform using shell execution
//     console.log("📦 Unpacking toolchain components...");
//     if (config.isZip) {
//         // Uses native Windows PowerShell utility via Bun Shell
//         await $`powershell -Command "Expand-Archive -Path '${downloadPath}' -DestinationPath '${INSTALL_DIR}' -Force"`;
//     } else {
//         // Linux and macOS native tar toolchain
//         await $`tar -xf ${downloadPath} -C ${INSTALL_DIR}`;
//     }

//     // Cleanup download file
//     await $`rm ${downloadPath}`.throws(false);

//     // 5. Fire system registry/shell append operations
//     await updateSystemPath(finalExecutableDir);
// }

// run().catch((err) => console.error("🚨 Setup failed:", err.message));

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
            // Fetch current User PATH from registry safely using Windows command line
            const currentPath = process.env.PATH || "";

            if (currentPath.includes(targetDir)) {
                console.log("Zig directory is already present in your Windows PATH.");
                return;
            }

            // setx appends permanently to the Windows User PATH registry key
            await $`setx PATH "%PATH%;${targetDir};${currentPath}"`;
            console.log("Successfully updated Windows PATH Registry.");
            console.log("Restart your terminal or IDE for the 'zig' command to activate.");
        } catch (err) {
            console.error("Failed to update Windows registry path:", err);
        }
    } else {
        // macOS (darwin) and Linux use text file shell configurations
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

        // Append the export string syntax to the end of the shell config file
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

await installZig("E:\\test");