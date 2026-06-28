import { arch, platform } from 'os';
import { existsSync, statSync } from 'node:fs';

export function getOS(): "win" | "linux" | "macos" {
    switch (platform()) {
        case "win32": return "win";
        case "linux": return "linux";
        case "darwin": return "macos";
        default: throw new Error("Unsupport OS")
    }
}

export function getArch(): "x64" | "arm64" {
    switch (arch()) {
        case "arm64": return "arm64";
        case "x64": return "x64";
        default: throw new Error("Unsuppoer Arch")
    }
}

export function getBinExt(): ".exe" | "" {
    switch (getOS()) {
        case "win": return ".exe";
        case "linux": return "";
        case "macos": return "";
        default: throw new Error("Unsupport OS")
    }
}

export function getLibExt(): ".dll" | ".so" | ".dylib" {
    switch (getOS()) {
        case "win": return ".dll";
        case "linux": return ".so";
        case "macos": return ".dylib";
        default: throw new Error("Unsupport OS")
    }
}

export function isGithubActions(): boolean {
    const GA = process.env["GITHUB_ACTIONS"];
    if (!GA) return false;
    if (typeof GA === "string") return GA.toLocaleLowerCase().trim() === "true";
    return GA;
}

export function isNewer(source: string, targets: string[]) {
    targets.forEach((target: string) => {
        if (!existsSync(target)) return true;
    })

    targets.forEach((target: string) => {
        if (statSync(source).mtimeMs > statSync(target).mtimeMs) return true
    })

    return false;
}

export async function checkPackage(packages: string[]): Promise<string[]> {
    if (getOS() !== "linux") throw new Error("Unsupport OS");
    const notInstalled: string[] = [];

    await Promise.all(
        packages.map(async (pkgName: string) => {
            let cmd = "";
            if (Bun.which("dpkg")) cmd = `dpkg -s ${pkgName}`;
            else if (Bun.which("rpm")) cmd = `rpm -q ${pkgName}`;
            else if (Bun.which("pacman")) cmd = `pacman -Q ${pkgName}`;
            else throw new Error("No package manager found");

            const process = Bun.spawnSync(cmd.split(" "), {
                stdout: 'ignore',
                stderr: 'ignore'
            });

            if (process.exitCode !== 0) {
                notInstalled.push(pkgName);
            }
        })
    )

    return notInstalled;
}

export async function installPackage(packages: string[]) {
    if (getOS() !== "linux") throw new Error("Unsupport OS");
    const installer = Bun.which("apt") ? "apt" : Bun.which("dnf") ? "dnf" : "pacman";
    const installCmd = ["install", "-y"];

    console.log(`Installing missing dependencies using ${installer}...`);
    await Bun.spawn(["sudo", installer, ...installCmd, ...packages], { stdout: "ignore", stderr: "ignore" }).exited;
}