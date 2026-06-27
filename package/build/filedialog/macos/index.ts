import { join, resolve } from "node:path"
import { spawnSync } from 'bun';
import { existsSync, mkdirSync } from "node:fs";
import { isNewer } from "../../utils/os";
/**
 * 
 * @param path 
 * path points to the directory that has filedialog.mm source code
 */
export default async function build_macos_filedialog(path: string) {
    path = resolve(path);
    const sourcePath = join(path, "filedialog.mm");
    if (!existsSync(sourcePath)) throw new Error(`filedialog.mm not found at ${sourcePath}`);

    const buildDir = join(path, "build");
    const outPath = join(buildDir, "filedialog.dylib");

    mkdirSync(buildDir, { recursive: true });

    if (isNewer(sourcePath, [outPath])) {
        spawnSync([
            "clang++",
            "-shared",
            "-fPIC",
            "-o", outPath, sourcePath,
            "-framework", "MediaPlayer",
            "-framework", "AppKit",
            "-framework", "Cocoa",
            "-std=c++11", "-O2", "-ObjC++"],
            {
                stdout: "ignore",
                stderr: "ignore"
            });

        spawnSync(["strip", "-S", outPath], {
            stdout: "ignore",
            stderr: "ignore"
        });
    }
    else {
        console.log("filedialog is unchanged, Skipping...")
    }
}