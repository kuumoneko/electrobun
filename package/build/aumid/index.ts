import { existsSync } from "node:fs";
import { join, resolve } from "node:path";
import { getOS } from "../../share/os";
import build_win_aumid from "./win";

/**
 * 
 * @param path 
 * path points to the directory that has aumid source code
 */
export default async function build_aumid(path: string) {
    path = resolve(path);
    if (!existsSync(path)) throw new Error("Invalid aumid path");

    const OS = getOS();
    const osaumid = join(path, OS);

    if (!existsSync(osaumid)) throw new Error(`${OS} aumid source not found at ${osaumid}`)

    switch (OS) {
        case "win": build_win_aumid(osaumid); break;
        default: throw new Error("Unsupported OS")
    }
}