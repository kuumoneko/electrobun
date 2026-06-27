import { existsSync } from "node:fs";
import { join, resolve } from "node:path";
import { getOS } from "../utils/os";
import build_linux_filedialog from "./linux";
import build_macos_filedialog from "./macos";
import build_win_filedialog from "./win";

/**
 * 
 * @param path 
 * path points to the directory that has filedialog source code
 */
export default async function build_filedialog(path: string) {
    path = resolve(path);
    if (!existsSync(path)) throw new Error("Invalid filedialog path");

    const OS = getOS();
    const osfiledialog = join(path, OS);

    if (!existsSync(osfiledialog)) throw new Error(`${OS} filedialog source not found at ${osfiledialog}`)

    switch (OS) {
        case "linux": build_linux_filedialog(osfiledialog); break;
        case "macos": build_macos_filedialog(osfiledialog); break;
        case "win": build_win_filedialog(osfiledialog); break;
        default: throw new Error("Unsupported OS")
    }
}