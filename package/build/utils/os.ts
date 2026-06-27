import { arch, platform } from 'os';

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
    const GA = process.env.GITHUB_ACTIONS;
    if (!GA) return false;
    if (typeof GA === "string") return GA.toLocaleLowerCase().trim() === "true";
    return GA;
}