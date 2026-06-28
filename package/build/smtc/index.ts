import { existsSync } from "node:fs";
import { join, resolve } from "node:path";
import { getOS } from "../../share/os";
import build_linux_smtc from "./linux";
import build_macos_smtc from "./macos";
import build_win_smtc from "./win";

/**
 * 
 * @param path 
 * path points to the directory that has smtc source code
 */
export default async function build_smtc(path: string) {
    path = resolve(path);
    if (!existsSync(path)) throw new Error("Invalid smtc path");

    const OS = getOS();
    const osSMTC = join(path, OS);

    if (!existsSync(osSMTC)) throw new Error(`${OS} SMTC source not found at ${osSMTC}`)

    switch (OS) {
        case "linux": build_linux_smtc(osSMTC); break;
        case "macos": build_macos_smtc(osSMTC); break;
        case "win": build_win_smtc(osSMTC); break;
        default: throw new Error("Unsupported OS")
    }
}