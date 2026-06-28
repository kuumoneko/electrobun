import { existsSync } from "node:fs";
import { resolve } from "node:path";
import { getOS } from "../../share/os";
import build_linux_native from "./linux";
import build_macos_native from "./macos";
import build_win_native from "./win";

/**
 * 
 * @param path 
 * workspace path
 */
export default async function build_native(path: string) {
    path = resolve(path);
    if (!existsSync(path)) throw new Error("Invalid native path");

    switch (getOS()) {
        case "linux": build_linux_native(path); break;
        case "macos": build_macos_native(path); break;
        case "win": build_win_native(path); break;
        default: throw new Error("Unsupported OS")
    }
}