import { join, resolve } from "node:path";

/**
 * 
 * @param path 
 * path points to the directory that has launcher.ts source code
 */
export default async function buildLauncher(path: string) {
    path = resolve(path);
    const mainSource = join(path, "main.zig")
}