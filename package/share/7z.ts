import { getOS } from "./os";
import { $, which, spawnSync } from 'bun';
import { existsSync, mkdirSync } from 'node:fs';
import { dirname } from "node:path";

export async function ensure7zip(): Promise<string[]> {
    let zipPath = which("7z") || which("7zip");

    const Windowsget = async () => {
        try {
            const whereAlloc = await $`where 7z`.quiet();
            if (whereAlloc.exitCode === 0) {
                zipPath = whereAlloc.stdout.toString().trim().split(/\r?\n/).at(-1) ?? "";
                return zipPath
            }
            else {
                return ""
            }
        } catch {
            return ""
        }
    }

    if (!zipPath && getOS() === "win") zipPath = await Windowsget();

    if (!zipPath || zipPath.length === 0) {
        const OS = getOS();
        if (OS === "win") {
            const wingetPath = which("winget");
            if (wingetPath) {
                await $`${wingetPath} install 7zip.7zip --silent --accept-source-agreements --accept-package-agreements`.quiet();
            }
        } else if (OS === "linux") {
            await $`sudo apt install -y p7zip-full`.quiet();
        } else if (OS === "macos") {
            await $`brew install p7zip`.quiet();
        }
    }

    zipPath = which("7z") || which("7zip");
    if (!zipPath && getOS() === "win") zipPath = await Windowsget()


    if (!zipPath || zipPath.length === 0) throw new Error("7zip is not installed");
    if (getOS() === "win") {
        if (zipPath.includes("WindowsApps")) {
            return ["cmd", "/c", "7z"];
        }
        return [zipPath];
    }

    return [zipPath];
}

export async function decompress(source: string, out: string) {
    const zipPath = await ensure7zip();

    const { exitCode } = spawnSync([...zipPath, "--version"], { stdio: ["ignore", "ignore", "ignore"] });
    if (exitCode !== 0) throw new Error("7zip is not installed or not working");

    mkdirSync(out, { recursive: true });

    spawnSync([...zipPath, "x", source, `-o${out}`], { stdio: ["ignore", "ignore", "ignore"] });
    console.log(`Extracted to ${out}.`)
}

export async function compress(source: string, out: string, type: string) {
    const zipPath = await ensure7zip();

    const { exitCode } = spawnSync([...zipPath, "--version"], { stdio: ["ignore", "ignore", "ignore"] });
    if (exitCode !== 0) throw new Error("7zip is not installed or not working");

    if (!existsSync(source)) throw new Error(`Source dir is not found at ${source}`);

    const outdir = dirname(out);
    mkdirSync(outdir, { recursive: true });

    spawnSync([...zipPath, "a", `-t${type}`, out, source], { stdio: ["ignore", "ignore", "ignore"] });
    console.log(`Added ${source} into ${out}.`);
}

export async function createHash(source: string) {
    const zipPath = await ensure7zip();

    const { exitCode } = spawnSync([...zipPath, "--version"], { stdio: ["ignore", "ignore", "ignore"] });
    if (exitCode !== 0) throw new Error("7zip is not installed or not working");

    if (!existsSync(source)) throw new Error(`Source dir is not found at ${source}`);

    const data = spawnSync([...zipPath, "h", "-scrcSHA256", source]);
    const text = data.stdout.toString().split("\n");
    const hashedString = text.find(value => value.includes("SHA256 for data"))?.split("SHA256 for data:")[1]?.trim();
    return hashedString;
}